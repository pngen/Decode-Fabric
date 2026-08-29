#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/backend.hpp"
#include "decodefabric/clock.hpp"
#include "decodefabric/compatibility.hpp"
#include "decodefabric/device.hpp"
#include "decodefabric/error.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/state_desc.hpp"
#include "decodefabric/token.hpp"

namespace decodefabric {

// --- Member-level outcome ---------------------------------------------------
enum class MemberOutcomeKind : std::uint8_t {
  StepSuccessContinue = 0,     // one authoritative token was committed; continue
  StepSuccessTerminal = 1,     // terminal condition (EOS / max length / operator)
  Yielded = 2,                 // produced nothing this step, eligible later
  RetryableFailure = 3,        // transient; may be retried under a new attempt
  NonRetryableFailure = 4,     // permanent; sequence terminates
  Cancelled = 5,               // cancellation became authoritative at this step
  Expired = 6,                 // deadline became authoritative at this step
  StaleAuthorityRejected = 7,  // authority no longer current; no advance
};

const char* to_string(MemberOutcomeKind k) noexcept;

// The independent outcome of one member of a packed decode group. Packed-group
// members always retain independent outcomes; one failed member never
// invalidates an unrelated successful member.
struct MemberOutcome {
  SequenceId sequence;
  MemberOutcomeKind kind = MemberOutcomeKind::StepSuccessContinue;

  // Authority carried on the outcome so stale/duplicate completions are
  // deterministically rejected before any state advances.
  AttemptId attempt;
  DecodeGeneration generation;

  // For a successful step: how many tokens were committed (must be 1 for the
  // canonical step, or a declared larger quantum when the executor does).
  TokenCount generated = 0;
  std::uint64_t token_identifier = 0;  // optional generated token/value id
  bool terminal = false;

  // Failure detail (set for Retryable/NonRetryable/StaleAuthorityRejected).
  ErrorCode error_code = ErrorCode::Ok;
  std::string error_message;
  bool retryable = false;

  // Timing measured by the executor.
  TimePoint started_at;
  TimePoint finished_at;
  Nanoseconds active_ns = 0;

  // KV / memory update from this step (delta and post-step totals).
  std::uint64_t kv_bytes_delta = 0;
  std::uint64_t kv_bytes_after = 0;

  bool succeeded() const noexcept {
    return kind == MemberOutcomeKind::StepSuccessContinue ||
           kind == MemberOutcomeKind::StepSuccessTerminal;
  }
};

// --- Per-member execution spec ---------------------------------------------
struct DecodeMemberSpec {
  SequenceId sequence;
  AttemptId attempt;
  DecodeGeneration generation;   // authoritative generation this step advances
  StateDescriptor state;
  TokenCount current_length = 0;    // prompt + generated (start of step)
  TokenCount generated_tokens = 0;  // tokens before this step
  TokenCount remaining_budget = 0;  // tokens remaining before this step
  std::vector<std::uint8_t> payload;  // opaque executor-specific input (sampling
                                      // metadata, per-member tensor/state blobs)
};

// --- Decode execution request ----------------------------------------------
struct DecodeExecutionRequest {
  DispatchId dispatch_id;          // unique authority token for this dispatch
  CoordinatorEpoch epoch;          // coordinator epoch at dispatch time
  WorkerId worker;
  WorkerBootId worker_boot;
  CompatibilityKey key;
  DeviceDescriptor device;
  std::uint64_t reservation_id = 0;  // reservation this dispatch is charged to
  std::vector<DecodeMemberSpec> members;
  std::vector<std::uint8_t> group_payload;  // group-level opaque input
  Nanoseconds deadline_hint_ns = 0;
};

// --- Decode execution result ------------------------------------------------
// Retained for the pre-transactional completion path (e.g. a stale-replay
// artifact or a non-transactional backend error). Transactional workloads
// produce PreparedDecode / CommitGrant / MemberReceipt instead. A
// DecodeExecutionResult member outcome is never itself authoritative for a
// state transition: it either reflects a rejected/stale step or is folded into
// a receipt.
struct DecodeExecutionResult {
  DispatchId dispatch_id;
  // Authority under which the dispatch ran (used for stale-authority validation).
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  std::vector<MemberOutcome> outcomes;  // one per member, independent
  Nanoseconds group_active_ns = 0;      // measured backend wall time
  ErrorCode group_error = ErrorCode::Ok;    // only set for whole-group failure
  std::string group_error_message;
  bool group_retryable = false;
};

// --- Executor state transaction types --------------------------------------
// The executor holds, per StateId, a committed state and (while a transaction
// is in flight) a tentative/prepared state. A decode generation becomes
// authoritative only when the transition prepared from the current committed
// pre-state is authorized under current sequence/worker authority, committed
// exactly once, and bound by a receipt whose post-state becomes the next
// generation's pre-state.

// A single prepared, not-yet-committed member transition.
struct PreparedMember {
  SequenceId sequence;
  StateId state;
  AttemptId attempt;
  DecodeGeneration generation;
  DispatchId dispatch;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ProposalId proposal;          // unique one-use proposal/preparation identity
  std::uint64_t pre_state_digest = 0;   // digest of committed state before step
  std::uint64_t post_state_digest = 0;  // digest of proposed state after step
  std::uint64_t delta_digest = 0;       // optional deterministic transition digest
  MemberOutcome outcome;        // proposed outcome (token / terminal / kv)
  std::uint64_t committed_position_before = 0;  // authoritative committed-token index
  std::uint64_t committed_position_after = 0;   // index after this step
  Nanoseconds active_ns = 0;    // measured prepare (backend) time

  bool is_commit_eligible() const noexcept {
    return outcome.succeeded();
  }
};

// The result of one group-level prepare: an ordered set of per-member prepared
// transitions, one for each member of the dispatched group.
struct PreparedDecode {
  DispatchId dispatch_id;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  std::vector<PreparedMember> members;
  Nanoseconds group_active_ns = 0;
  ErrorCode group_error = ErrorCode::Ok;
  std::string group_error_message;
};

// A one-use commit grant. Issued by Decode Fabric only after full authority
// validation of the corresponding prepared transition. Bound to the complete
// authority/proposal tuple so it cannot be reused or applied to a different
// sequence, worker boot, epoch, attempt, generation, dispatch, or pre-state.
struct CommitGrant {
  GrantId grant_id;
  ProposalId proposal;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  SequenceId sequence;
  StateId state;
  AttemptId attempt;
  DecodeGeneration generation;
  DispatchId dispatch;
  std::uint64_t committed_position = 0;  // authoritative committed-token index
  std::uint64_t pre_state_digest = 0;
  std::uint64_t post_state_digest = 0;
  std::uint64_t delta_digest = 0;
  // Outcome metadata bound to the transition (for finalization).
  MemberOutcomeKind outcome_kind = MemberOutcomeKind::StepSuccessContinue;
  bool terminal = false;
  std::uint32_t token_identifier = 0;
  Nanoseconds active_ns = 0;
};

// The deterministic commit receipt binding a committed transition.
struct MemberReceipt {
  ReceiptId receipt_id;
  GrantId grant_id;
  ProposalId proposal;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  SequenceId sequence;
  StateId state;
  AttemptId attempt;
  DecodeGeneration generation;
  DispatchId dispatch;
  std::uint64_t committed_position_before = 0;
  std::uint64_t committed_position_after = 0;
  std::uint64_t pre_state_digest = 0;
  std::uint64_t post_state_digest = 0;
  std::uint64_t delta_digest = 0;
  MemberOutcomeKind outcome_kind = MemberOutcomeKind::StepSuccessContinue;
  bool terminal = false;
  std::uint32_t token_identifier = 0;
  Nanoseconds active_ns = 0;
  TimePoint committed_at;                 // timestamp metadata
};

// The canonical accepted-generation record: the durable, idempotent receipt of
// exactly one authorized state transition. When a generation is accepted (the
// fabric finalized it), this record IS the receipt. It binds the full existing
// authority tuple (CoordinatorEpoch, WorkerId, WorkerBootId, SequenceId,
// StateId, AttemptId, DecodeGeneration, DispatchId) plus the committed-token
// indices and the pre/post state digests, and carries a stable idempotency key.
// A replayed/recovered acceptance of the SAME generation is reconciled exactly
// once instead of minting or authorizing a second logical transition.
// `promotion_observed` records whether the executor's committed-state promotion
// was confirmed by a receipt at acceptance time; when false it represents the
// recovery edge "Fabric accepted, executor promotion not yet observed", and
// recovery reconciles that SAME generation exactly once rather than authorizing
// a second one.
struct AcceptedGeneration {
  std::uint64_t idempotency_key = 0;   // stable hash over the authority identity
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  SequenceId sequence;
  StateId state;
  AttemptId attempt;
  DecodeGeneration generation;
  DispatchId dispatch;
  std::uint64_t committed_position_before = 0;
  std::uint64_t committed_position_after = 0;
  std::uint64_t pre_state_digest = 0;
  std::uint64_t post_state_digest = 0;
  std::uint64_t delta_digest = 0;
  bool terminal = false;
  bool promotion_observed = false;
};

// A group-level decode result: per-member receipts for members that committed.
struct ReceiptDecode {
  DispatchId dispatch_id;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  std::vector<MemberReceipt> receipts;
  ErrorCode group_error = ErrorCode::Ok;
  std::string group_error_message;
};

// An explicit abort/discard of a prepared (but never committed) transition.
struct AbortPrepared {
  ProposalId proposal;
  SequenceId sequence;
  StateId state;
  AttemptId attempt;
  DecodeGeneration generation;
  DispatchId dispatch;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
};

// --- Executor contract ------------------------------------------------------
// A DecodeExecutor executes exactly one authoritative decode iteration (one
// "quantum") for a packed group of compatible sequences, but through a
// transactional state protocol: prepare (read-only), then commit (on a one-use
// grant) or abort. Implementations may process the group as a single backend
// call. Members keep independent outcomes. Implementations are synchronous; the
// caller (scheduler on a worker thread, or a worker process over the control
// plane) decides how to schedule the call.
//
// The fundamental invariant: NO executor-resident state transition becomes
// committed/durable unless it is authorized by the exact current Decode Fabric
// authority for that sequence step.
class DecodeExecutor {
 public:
  virtual ~DecodeExecutor() = default;

  virtual ExecutorId id() const = 0;
  virtual BackendKind backend() const = 0;
  virtual DeviceDescriptor device() const = 0;

  // Whether this executor can run a request with the given key.
  virtual bool supports(const CompatibilityKey& key) const = 0;

  // Prepare phase. Reads ONLY the current committed state, computes the
  // proposed next step into tentative storage, leaves committed state
  // unchanged, and returns one PreparedMember per input member. Must not
  // silently overwrite an unresolved prepared transition for the same
  // authoritative sequence generation (it should reject it instead).
  virtual Result<PreparedDecode> prepare(const DecodeExecutionRequest& req) = 0;

  // Commit phase. Receives one one-use commit grant, locates the exact prepared
  // transition, verifies proposal + pre/post digests + full grant binding, then
  // atomically promotes tentative -> committed, marks the proposal and grant
  // consumed, and returns a receipt. Duplicate use of an already-consumed grant
  // is idempotent (returns the existing receipt when identity matches exactly)
  // and never applies the transition twice.
  virtual Result<MemberReceipt> commit(const CommitGrant& grant) = 0;

  // Abort phase. Discards the matching prepared transition (and any
  // associated tentative resources). Committed state is unchanged.
  virtual Result<void> abort(const AbortPrepared& abort) = 0;

  // Best-effort cancellation of in-flight work. The required safe boundary is
  // between decode iterations; implementations that cannot interrupt a kernel
  // mid-flight may return false, and the fabric cancels at the next boundary.
  virtual bool cancel() { return false; }
};

}  // namespace decodefabric
