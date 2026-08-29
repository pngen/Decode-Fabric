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
// It implements the transactional prepare/commit/abort state protocol: prepare
// reads only the committed state and computes the proposed next step into
// tentative storage; commit atomically promotes tentative -> committed on a
// one-use grant and returns a deterministic receipt; abort discards tentative
// state. No committed executor-resident state is ever mutated except by a
// validated commit.
class CpuDecodeExecutor final : public DecodeExecutor {
 public:
  explicit CpuDecodeExecutor(DeviceId device_id = DeviceId::from(1));
  ~CpuDecodeExecutor() override = default;

  ExecutorId id() const override;
  BackendKind backend() const override;
  DeviceDescriptor device() const override;
  bool supports(const CompatibilityKey& key) const override;

  Result<PreparedDecode> prepare(const DecodeExecutionRequest& req) override;
  Result<MemberReceipt> commit(const CommitGrant& grant) override;
  Result<void> abort(const AbortPrepared& abort) override;

  // Determinism/verification helpers (exposed for tests and benchmarks).
  static std::uint64_t kStateSize;
  static std::vector<double> initial_state(StateId id, std::uint64_t seed);
  static std::uint64_t state_checksum(const std::vector<double>& s);
  static std::uint32_t step_token(const std::vector<double>& s);

  // Deterministic digest over the committed state identity (recurrent state plus
  // the state fields that define execution identity) for a StateId.
  std::uint64_t committed_state_digest(StateId id) const;

  // State-introspection test hooks (read-only; never allow unsafe mutation of
  // committed state from the public surface).
  bool has_prepared(StateId id, ProposalId proposal) const;
  std::uint64_t tentative_tokens(StateId id) const;
  std::uint64_t committed_tokens(StateId id) const;

 private:
  struct SeqState {
    std::vector<double> state;        // committed recurrent state
    std::uint64_t tokens = 0;         // committed token count
    std::uint64_t kv_bytes = 0;       // committed KV-like bytes
    bool eos_enabled = false;
    std::uint32_t eos_target = 0;
    // Tentative/prepared transition (unresolved until commit).
    bool has_tentative = false;
    std::vector<double> tentative_state;
    std::uint64_t tentative_kv_bytes = 0;
    ProposalId prepared_proposal;
    AttemptId prepared_attempt;
    DecodeGeneration prepared_generation;
    SequenceId prepared_sequence;
    WorkerId prepared_worker;
    WorkerBootId prepared_worker_boot;
    CoordinatorEpoch prepared_epoch;
    DispatchId prepared_dispatch;
    std::uint64_t prepared_pre_digest = 0;
    std::uint64_t prepared_post_digest = 0;
  };

  Result<PreparedMember> prepare_member(const DecodeMemberSpec& m,
                                          const DecodeExecutionRequest& req,
                                          Nanoseconds* active_ns);
  Result<void> ensure_initialized(SeqState& st, const DecodeMemberSpec& m);
  static void clear_tentative(SeqState& st);

  mutable std::mutex mu_;
  std::unordered_map<StateId, SeqState> states_;
  // Idempotence ledger: consumed grant -> committed receipt (for duplicate
  // commit of an already-applied transition).
  std::unordered_map<GrantId, MemberReceipt> receipts_by_grant_;
  std::unordered_map<ProposalId, bool> consumed_proposals_;   // committed or aborted proposal
  std::uint64_t next_proposal_ = 0;
  DeviceId device_id_;
  ExecutorId id_;
  DeviceDescriptor device_;
};

}  // namespace decodefabric
