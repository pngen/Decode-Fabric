#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "decodefabric/executor.hpp"

namespace decodefabric {

// A real, deterministic CPU decode executor. It performs bounded, stateful
// numerical work (a dimensionality-reduced "model" recurrence) whose results
// are reproducible and independently verifiable. It never pretends to execute
// with sleep/counters/random values: each authoritative step transforms a
// per-sequence state vector using real numeric operations, and the previous
// step's state influences the next token.
//
// This backend is one concrete accelerator. CUDA is a separate backend; the
// runtime type system never assumes CPU.
class CpuDecodeExecutor final : public DecodeExecutor {
 public:
  explicit CpuDecodeExecutor(DeviceId device_id = DeviceId::from(1));
  ~CpuDecodeExecutor() override = default;

  ExecutorId id() const override;
  BackendKind backend() const override;
  DeviceDescriptor device() const override;
  bool supports(const CompatibilityKey& key) const override;
  Result<DecodeExecutionResult> execute(const DecodeExecutionRequest& req) override;

  // Determinism/verification helpers (exposed for tests and benchmarks).
  static std::uint64_t kStateSize;
  static std::vector<double> initial_state(StateId id, std::uint64_t seed);
  static std::uint64_t state_checksum(const std::vector<double>& s);
  static std::uint32_t step_token(const std::vector<double>& s);

 private:
  struct SeqState {
    std::vector<double> state;
    std::uint64_t tokens = 0;
    std::uint64_t kv_bytes = 0;
    bool eos_enabled = false;
    std::uint32_t eos_target = 0;
  };
  Result<DecodeExecutionResult> run_member(const DecodeMemberSpec& m, Nanoseconds* active_ns);

  std::mutex mu_;
  std::unordered_map<StateId, SeqState> states_;
  DeviceId device_id_;
  ExecutorId id_;
  DeviceDescriptor device_;
};

}  // namespace decodefabric
