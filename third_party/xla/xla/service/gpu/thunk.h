/* Copyright 2017 The OpenXLA Authors.

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

#ifndef XLA_SERVICE_GPU_THUNK_H_
#define XLA_SERVICE_GPU_THUNK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "xla/executable_run_options.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/gpu/buffer_allocations.h"
#include "xla/service/gpu/gpu_executable_run_options.h"
#include "xla/service/service_executable_run_options.h"
#include "xla/status.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor.h"

namespace xla {
namespace gpu {

class GpuExecutable;

// Thunk acts as the bridge between IrEmitter and GpuExecutable. It stores the
// metadata IrEmitter generates for GpuExecutable to invoke an HloInstruction.
//
// Thunk provides the Initialize and ExecuteOnStream interface for GpuExecutable
// to initialize and execute the invocation respectively. Its subclasses are
// supposed to override these interfaces to launch a generated kernel or call an
// external library function (such as operations in cuBLAS).
//
// This is thread-compatible.
class Thunk {
 public:
  enum Kind {
    kCholesky,
    kConditional,
    kConvolution,
    kConvolutionReorder,
    kCopy,
    kCommandBuffer,
    kCubSort,
    kCublasLtMatmul,
    kCustomCall,
    kCustomKernel,
    kFft,
    kFor,
    kGemm,
    kInfeed,
    kKernel,
    kMemset32BitValue,
    kMemzero,
    kNcclAllGather,
    kNcclAllGatherStart,
    kNcclAllGatherDone,
    kNcclAllReduce,
    kNcclAllReduceStart,
    kNcclAllReduceDone,
    kNcclCollectivePermute,
    kNcclCollectivePermuteStart,
    kNcclCollectivePermuteDone,
    kNcclReduceScatter,
    kNcclReduceScatterStart,
    kNcclReduceScatterDone,
    kNcclAllToAll,
    kNcclAllToAllStart,
    kNcclAllToAllDone,
    kNcclSend,
    kNcclRecv,
    kNorm,
    kOutfeed,
    kPartitionId,
    kRecv,
    kRecvDone,
    kReplicaId,
    kSequential,
    kSend,
    kSendDone,
    kTriangularSolve,
    kWhile,
    kFusedMHA
  };

  // TODO(ezhulenev): This should become a part of StreamExecutor library, but
  // for now we keep it here as a Thunk implementation detail. It's not yet
  // clear what else should become a part of "executable source", we likely
  // need to keep some information about available symbols and signatures.
  struct ExecutableSource {
    std::string_view text;             // PTX for NVIDIA backend
    absl::Span<const uint8_t> binary;  // CUBIN for NVIDIA backends
  };

  struct ThunkInfo {
    explicit ThunkInfo(mlir::Operation* op) : op(op) {}
    static ThunkInfo WithProfileAnnotation(mlir::Operation* op);
    static ThunkInfo WithProfileAnnotation(const HloInstruction* instr);

    std::string profile_annotation;
    // TODO(b/304613751): This is only needed by the LMHLO. Remove this when
    // LMHLO is removed from the runtime pipeline.
    mlir::Operation* op;
  };

  // Parameters passed to Initialize. At thunk initialization time we do not
  // launch any "work" on device and only prepare thunks for execution, i.e.
  // we pre-load kernels on device and instantiate all command buffers.
  struct InitializeParams {
    se::StreamExecutor* executor = nullptr;
    ExecutableSource src;

    const BufferAllocations* buffer_allocations = nullptr;

    // Main compute stream that will be used, passed via `ExecuteParams` to
    // `ExecuteOnStream`. It can be used to initialize on-device "state" (i.e.
    // various control structures) at command buffer recording time (we use it
    // to initialize NCCL execution plans on device when we trace NCCL
    // operations into command buffers);
    se::Stream* stream = nullptr;

    // Auxiliary stream for tracing command buffers. We use a separate stream to
    // avoid accidental tracing of unrelated activities on a main stream.
    se::Stream* command_buffer_trace_stream = nullptr;

    const NcclExecuteParams* nccl_params = nullptr;
  };

  // Parameters passed to ExecuteOnStream. ExecuteOnStream is responsible for
  // launching "work" on device, i.e. it launches kernels, executes command
  // buffers and calls into libraries (cuBLAS, cuDNN etc.).
  struct ExecuteParams {
    ExecuteParams(const ServiceExecutableRunOptions& run_options,
                  const BufferAllocations& buffer_allocations,
                  se::Stream* stream, se::Stream* command_buffer_trace_stream,
                  absl::Span<se::Stream* const> async_streams);

    const BufferAllocations* buffer_allocations;  // never null

    // Main compute stream on which thunks launch operations.
    se::Stream* stream;

    // Auxiliary stream for tracing command buffers. We use a separate stream to
    // avoid accidental tracing of unrelated activities on a main stream.
    se::Stream* command_buffer_trace_stream;

    // Streams for asynchronous collective communications.
    absl::InlinedVector<se::Stream*, 4> async_comms_streams;

    NcclExecuteParams nccl_params;

    // Streams for moving data between host and device.
    se::Stream* device_to_host_stream;
    se::Stream* host_to_device_stream;

    // Send/Recv callbacks passed to XLA from PjRt.
    SendDeviceMemoryFunction* send_device_memory_function;
    RecvDeviceMemoryFunction* recv_device_memory_function;
  };

  // The hlo_instruction argument is meant to be the instruction this thunk was
  // generated from, but Thunk never uses this argument other than to save it
  // to Thunk::hlo_instruction, so it can be null.
  Thunk(Kind kind, ThunkInfo thunk_info)
      : kind_(kind),
        profile_annotation_(thunk_info.profile_annotation),
        op_(thunk_info.op) {}
  virtual ~Thunk() = default;
  Thunk(const Thunk&) = delete;
  Thunk& operator=(const Thunk&) = delete;

  virtual std::string ToStringExtra(int indent) const { return ""; }
  Kind kind() const { return kind_; }
  std::string profile_annotation() const { return profile_annotation_; }

  // Only valid during compilation, i.e., lowering thunks to kernel-launch
  // related XLA runtime custom calls). nullptr at runtime. MLIR codegen will
  // cease the practice of lowering thunks to XLA runtime custom calls.
  mlir::Operation* op() { return op_; }

  // Prepares the thunk for execution on the given StreamExecutor.
  //
  // This may be called multiple times.  Its main purpose is to give us a chance
  // to do initialization outside of ExecuteOnStream() so that the
  // time spent initializing doesn't count towards our execution profile.
  virtual absl::Status Initialize(const InitializeParams& params) {
    return absl::OkStatus();
  }

  // Execute the kernel for the thunk on the given stream. This method must be
  // called after Initialize and can be called multiple times over Thunk's
  // lifetime.
  //
  // Precondition: Initialize(stream->parent()) has been called.
  virtual absl::Status ExecuteOnStream(const ExecuteParams& params) = 0;

  // Clears metadata that is only valid during compile time.
  virtual void ClearCompileTimeInfo() { op_ = nullptr; }

  static absl::string_view KindToString(Thunk::Kind kind);

 private:
  Kind kind_;
  std::string profile_annotation_;
  mlir::Operation* op_;
};

// A sequence of thunks.
class ThunkSequence : public std::vector<std::unique_ptr<Thunk>> {
 public:
  std::string ToString(int indent = 0,
                       std::function<std::string(const Thunk*)>
                           get_thunk_annotation = nullptr) const;
};

std::ostream& operator<<(std::ostream& os, Thunk::Kind kind);

// A struct that defines a shaped slice, i.e., a BufferAllocation::Slice and its
// shape.
struct ShapedSlice {
  BufferAllocation::Slice slice;
  Shape shape;
};

// Returns if the thunk implements a reduction collective (all-reduce or
// reduce-scatter).
bool IsReductionCollective(Thunk::Kind kind);
}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_THUNK_H_
