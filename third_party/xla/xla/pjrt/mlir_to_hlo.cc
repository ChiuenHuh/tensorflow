/* Copyright 2021 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/pjrt/mlir_to_hlo.h"

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Bytecode/BytecodeWriter.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/MLProgram/IR/MLProgram.h"  // from @llvm-project
#include "mlir/Dialect/Shape/IR/Shape.h"  // from @llvm-project
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "stablehlo/dialect/ChloOps.h"  // from @stablehlo
#include "stablehlo/dialect/Register.h"  // from @stablehlo
#include "stablehlo/dialect/StablehloOps.h"  // from @stablehlo
#include "xla/mlir/utils/error_util.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/mlir_hlo/mhlo/IR/register.h"
#include "xla/mlir_hlo/mhlo/transforms/passes.h"
#include "xla/statusor.h"
#include "xla/translate/mhlo_to_hlo/mlir_hlo_to_hlo.h"

namespace xla {

static mlir::Attribute ArrayToElements(mlir::Attribute attr) {
  if (auto array = attr.dyn_cast<mlir::DenseI64ArrayAttr>()) {
    return mlir::DenseIntElementsAttr::get(
        mlir::RankedTensorType::get(array.size(), array.getElementType()),
        array.asArrayRef());
  }
  return attr;
}

static mlir::Attribute ElementsToArray(mlir::Attribute attr) {
  if (auto elements = llvm::dyn_cast<mlir::DenseIntElementsAttr>(attr)) {
    return mlir::DenseI64ArrayAttr::get(
        attr.getContext(), llvm::to_vector(elements.getValues<int64_t>()));
  }
  return attr;
}

// Convert attrs that use DenseI64ArrayAttr to use a different type of
// Attribute. For backwards compatibility purposes, arrays should be converted
// to DenseIntElementsAttr right before serialization, and converted back right
// after serialization. Deserialization checks the IR is valid by default, so
// you will need to disable that and do the verification explicitly after
// parsing.
void ConvertStablehloDenseAttributes(
    mlir::Operation* op,
    llvm::function_ref<mlir::Attribute(mlir::Attribute)> convert) {
  llvm::TypeSwitch<mlir::Operation*>(op)
      .Case([&](mlir::stablehlo::BroadcastOp op) {
        op->setAttr("broadcast_sizes", convert(op->getAttr("broadcast_sizes")));
      })
      .Case([&](mlir::stablehlo::DynamicSliceOp op) {
        op->setAttr("slice_sizes", convert(op->getAttr("slice_sizes")));
      })
      .Case([&](mlir::stablehlo::FftOp op) {
        op->setAttr("fft_length", convert(op->getAttr("fft_length")));
      })
      .Case([&](mlir::stablehlo::PadOp op) {
        op->setAttr("edge_padding_low",
                    convert(op->getAttr("edge_padding_low")));
        op->setAttr("edge_padding_high",
                    convert(op->getAttr("edge_padding_high")));
        op->setAttr("interior_padding",
                    convert(op->getAttr("interior_padding")));
      })
      .Case([&](mlir::stablehlo::ReverseOp op) {
        op->setAttr("dimensions", convert(op->getAttr("dimensions")));
      })
      .Case([&](mlir::stablehlo::SliceOp op) {
        op->setAttr("start_indices", convert(op->getAttr("start_indices")));
        op->setAttr("limit_indices", convert(op->getAttr("limit_indices")));
        op->setAttr("strides", convert(op->getAttr("strides")));
      })
      .Case([&](mlir::stablehlo::TransposeOp op) {
        op->setAttr("permutation", convert(op->getAttr("permutation")));
      });
}

Status MlirToXlaComputation(mlir::ModuleOp module,
                            XlaComputation& xla_computation,
                            bool use_tuple_args, bool return_tuple,
                            bool legalize_sparse_ops) {
  mlir::BaseScopedDiagnosticHandler diagnostic_handler(module->getContext());
  {
    mlir::PassManager pm(module->getContext());
    if (legalize_sparse_ops) {
      // Convert sparse operations to custom_calls in order to translate sparse
      // operations into XLA HLO.
      pm.addNestedPass<mlir::func::FuncOp>(
          mlir::mhlo::createLegalizeSparseOperationsPass(
              /*legalizeToCustomCalls=*/true));
    }
    pm.addPass(mlir::mhlo::createStablehloLegalizeToHloPass());
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::mhlo::createChloLegalizeToHloPass(
            /*legalizeBroadcasts=*/true, /*expandCompositions=*/true));
    pm.addNestedPass<mlir::func::FuncOp>(mlir::createCanonicalizerPass());
    // In order to export to XLA, we must sink constants to control flow
    // regions, since XLA uses functional control flow.
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::mhlo::createSinkConstantsToControlFlowPass());
    if (failed(pm.run(module))) {
      VLOG(1) << "MHLO->HLO lowering passes failed.";
      module->dump();
      return diagnostic_handler.ConsumeStatus();
    }

    VLOG(5) << "MHLO module after lowering, before HLO import ";
    if (VLOG_IS_ON(5)) {
      module->dump();
    }
  }

  HloProto proto;
  mlir::MlirToHloConversionOptions options;
  TF_RETURN_IF_ERROR(ConvertMlirHloToHlo(module, &proto, use_tuple_args,
                                         return_tuple, options));

  xla_computation = XlaComputation(std::move(*proto.mutable_hlo_module()));
  return OkStatus();
}

StatusOr<std::string> SerializeModule(mlir::ModuleOp module) {
  std::string bytecode;
  llvm::raw_string_ostream os(bytecode);
  mlir::BytecodeWriterConfig config;
  // Pin bytecode version to 1 until transition to stable.
  // TODO: b/285913864 - Remove post enabling frameworks to set it.
  config.setDesiredBytecodeVersion(1);
  DowngradeStablehlo(module);
  if (mlir::failed(mlir::writeBytecodeToFile(module, os, config))) {
    return absl::InvalidArgumentError("mlir::writeBytecodeToFile failed");
  }
  return bytecode;
}

StatusOr<mlir::OwningOpRef<mlir::ModuleOp>> ParseMlirModuleString(
    absl::string_view mlir_module_str, mlir::MLIRContext& context) {
  mlir::OwningOpRef<mlir::ModuleOp> module;

  mlir::DialectRegistry registry;
  registry.insert<mlir::arith::ArithDialect>();
  registry.insert<mlir::func::FuncDialect>();
  registry.insert<mlir::ml_program::MLProgramDialect>();
  registry.insert<mlir::shape::ShapeDialect>();
  mlir::mhlo::registerAllMhloDialects(registry);
  mlir::stablehlo::registerAllDialects(registry);
  mlir::func::registerAllExtensions(registry);
  context.appendDialectRegistry(registry);

  mlir::BaseScopedDiagnosticHandler diagnostic_handler(&context);
  module = mlir::parseSourceString<mlir::ModuleOp>(
      llvm::StringRef(mlir_module_str.data(), mlir_module_str.size()),
      // IR may be invalid because some fields may be using DenseElements
      // instead of DenseArray. We rectify that below and verify after.
      mlir::ParserConfig{&context, /*verifyAfterParse=*/false});
  if (!module) {
    return diagnostic_handler.ConsumeStatus();
  }

  UpgradeStablehlo(*module);

  if (failed(module->verifyInvariants())) {
    VLOG(1) << "MLIR verification failed.";
    module->dump();
    return diagnostic_handler.ConsumeStatus();
  }
  return std::move(module);
}

Status ParseMlirModuleStringAndConvertToXlaComputation(
    absl::string_view mlir_module_str, XlaComputation& xla_computation,
    bool use_tuple_args, bool return_tuple) {
  mlir::MLIRContext context;
  TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> module,
                      xla::ParseMlirModuleString(mlir_module_str, context));
  return xla::MlirToXlaComputation(*module, xla_computation, use_tuple_args,
                                   return_tuple);
}

void DowngradeStablehlo(mlir::ModuleOp module) {
  module->walk([](mlir::Operation* op) {
    ConvertStablehloDenseAttributes(op, ArrayToElements);
  });
}

void UpgradeStablehlo(mlir::ModuleOp module) {
  module->walk([](mlir::Operation* op) {
    ConvertStablehloDenseAttributes(op, ElementsToArray);
  });
}

}  // namespace xla
