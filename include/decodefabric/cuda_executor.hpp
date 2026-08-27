#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>
#include "decodefabric/executor.hpp"

namespace decodefabric {

// A real CUDA decode backend. It selects a real device, establishes a CUDA
// context, allocates real host/device memory, transfers real inputs, launches a
// real kernel for a bounded, stateful decode-like recurrence, synchronizes,
// copies results back, verifies them, and frees all allocations. It is a
// concrete accelerator backend (one of several); the runtime type system never
// assumes CUDA.
//
// Per-sequence state lives in persistent device memory so a previous step's
// output genuinely affects the next step.
class CudaDecodeExecutor final : public DecodeExecutor {
 public:
  explicit CudaDecodeExecutor(DeviceId device_id = DeviceId::from(2), int cuda_device = 0);
  ~CudaDecodeExecutor() override;

  ExecutorId id() const override;
  BackendKind backend() const override;
  DeviceDescriptor device() const override;
  bool supports(const CompatibilityKey& key) const override;
  Result<DecodeExecutionResult> execute(const DecodeExecutionRequest& req) override;

  static constexpr std::uint64_t kStateSize = 8;

  // Helpers for tests/benchmarks.
  static std::vector<float> initial_state(StateId id, std::uint64_t seed);
  static std::uint64_t state_checksum(const std::vector<float>& s);
  static std::uint32_t step_token(const std::vector<float>& s);

 private:
  struct DevState {
    float* device = nullptr;
    std::uint64_t kv_bytes = 0;
    std::uint32_t token = 0;
    std::uint64_t checksum = 0;
    bool inited = false;
  };
  Result<DecodeExecutionResult> run_member(const DecodeMemberSpec& m, Nanoseconds* active_ns);

  std::mutex mu_;
  std::map<StateId, DevState> states_;
  DeviceId device_id_;
  ExecutorId id_;
  DeviceDescriptor device_;
  int cuda_device_ = 0;
};

}  // namespace decodefabric
