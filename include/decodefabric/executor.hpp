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

// --- Executor contract ------------------------------------------------------
// A DecodeExecutor executes exactly one authoritative decode iteration (one
// "quantum") for a packed group of compatible sequences. Implementations may
// process the group as a single backend call. Members keep independent
// outcomes. The interface is synchronous: the caller (scheduler on a worker
// thread, or a worker process over the control plane) decides how to schedule
// the call.
class DecodeExecutor {
 public:
  virtual ~DecodeExecutor() = default;

  virtual ExecutorId id() const = 0;
  virtual BackendKind backend() const = 0;
  virtual DeviceDescriptor device() const = 0;

  // Whether this executor can run a request with the given key.
  virtual bool supports(const CompatibilityKey& key) const = 0;

  // Execute one authoritative iterate. Must return a per-member outcome for
  // each member and must not advance anything on its own: advancement is the
  // sole responsibility of the fabric, which applies outcome semantics.
  virtual Result<DecodeExecutionResult> execute(const DecodeExecutionRequest& req) = 0;

  // Best-effort cancellation of in-flight work. The required safe boundary is
  // between decode iterations; implementations that cannot interrupt a kernel
  // mid-flight may return false, and the fabric cancels at the next boundary.
  virtual bool cancel() { return false; }
};

}  // namespace decodefabric
