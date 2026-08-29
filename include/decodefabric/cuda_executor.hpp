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
// output genuinely affects the next step. It implements the transactional
// prepare/commit/abort protocol with explicit committed (st.device) and
// tentative (st.tentative) device buffers: prepare reads only the committed
// buffer and writes the proposed next state into the tentative buffer; commit
// atomically promotes tentative -> committed on a one-use grant; abort frees
// the tentative buffer and leaves committed state unchanged.
class CudaDecodeExecutor final : public DecodeExecutor {
 public:
  explicit CudaDecodeExecutor(DeviceId device_id = DeviceId::from(2), int cuda_device = 0);
  ~CudaDecodeExecutor() override;

  ExecutorId id() const override;
  BackendKind backend() const override;
  DeviceDescriptor device() const override;
  bool supports(const CompatibilityKey& key) const override;

  Result<PreparedDecode> prepare(const DecodeExecutionRequest& req) override;
  Result<MemberReceipt> commit(const CommitGrant& grant) override;
  Result<void> abort(const AbortPrepared& abort) override;

  static constexpr std::uint64_t kStateSize = 8;

  // Helpers for tests/benchmarks.
  static std::vector<float> initial_state(StateId id, std::uint64_t seed);
  static std::uint64_t state_checksum(const std::vector<float>& s);
  static std::uint32_t step_token(const std::vector<float>& s);

  // Deterministic digest over the committed device-resident state identity.
  // The device buffer is copy-backed (bounded) to host for hashing; the digest
  // combines the raw float checksum with the committed token/kv counters.
  std::uint64_t committed_state_digest(StateId id) const;

  // Read-only introspection hooks for the CUDA transactional proof.
  bool has_prepared(StateId id, ProposalId proposal) const;
  std::uint64_t committed_tokens(StateId id) const;
  std::uint64_t dev_committed_checksum(StateId id) const;

 private:
  struct DevState {
    float* device = nullptr;      // committed persistent device buffer
    float* tentative = nullptr;   // tentative device buffer (during prepare)
    std::uint64_t kv_bytes = 0;   // committed KV-like bytes
    std::uint64_t tokens = 0;     // committed token count
    std::uint32_t token = 0;      // last committed token
    std::uint64_t checksum = 0;   // last committed checksum
    bool inited = false;
    // Tentative/prepared metadata.
    bool has_tentative = false;
    ProposalId prepared_proposal;
    AttemptId prepared_attempt;
    DecodeGeneration prepared_generation;
    SequenceId prepared_sequence;
    WorkerId prepared_worker;
    WorkerBootId prepared_worker_boot;
    CoordinatorEpoch prepared_epoch;
    DispatchId prepared_dispatch;
    std::uint64_t tentative_kv_bytes = 0;
    std::uint32_t tentative_token = 0;
    std::uint64_t tentative_checksum = 0;
    std::uint64_t prepared_pre_digest = 0;
    std::uint64_t prepared_post_digest = 0;
  };

  Result<PreparedMember> prepare_member(const DecodeMemberSpec& m,
                                          const DecodeExecutionRequest& req,
                                          Nanoseconds* active_ns);
  Result<void> ensure_initialized(DevState& st, const DecodeMemberSpec& m);
  static void clear_tentative(DevState& st);

  mutable std::mutex mu_;
  std::map<StateId, DevState> states_;
  std::unordered_map<GrantId, MemberReceipt> receipts_by_grant_;
  std::unordered_map<ProposalId, bool> consumed_proposals_;
  std::uint64_t next_proposal_ = 0;
  DeviceId device_id_;
  ExecutorId id_;
  DeviceDescriptor device_;
  int cuda_device_ = 0;
};

}  // namespace decodefabric
