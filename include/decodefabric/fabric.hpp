#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "decodefabric/clock.hpp"
#include "decodefabric/error.hpp"
#include "decodefabric/explain.hpp"
#include "decodefabric/executor.hpp"
#include "decodefabric/group.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/observability.hpp"
#include "decodefabric/persistence.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/worker.hpp"

namespace decodefabric {

// Global limits applied at admission and scheduling. Zero fields mean
// "unbounded" except where noted.
struct FabricLimits {
  std::uint32_t max_active_sequences = 0;      // 0 = unbounded
  std::uint64_t max_outstanding_tokens = 0;    // 0 = unbounded (aggregate gen budget)
  std::uint64_t tokens_per_cycle_budget = 0;   // 0 = unbounded (per scheduling cycle)
  std::uint64_t memory_headroom_bytes = 0;     // capacity reserve floor
  std::uint32_t max_ready_candidates = 0;      // 0 = unbounded (ready set cap)
};

// Retry policy. Retries always mint a NEW AttemptId; committed tokens remain
// committed (no implicit rollback) unless the operator explicitly configures a
// recovery point that discards them.
struct RetryPolicy {
  std::uint32_t max_attempts = 3;               // initial attempt is #1
  Nanoseconds base_backoff_ns = 1000000;        // 1 ms between attempts
  bool preserve_committed_tokens = true;        // never roll back committed steps
};

// Outcome of admission.
struct AdmissionDecision {
  bool admitted = false;
  RequestId request;
  SequenceId sequence;
  std::string reason;                 // structured textual reason
  std::vector<std::string> facts;     // atomic admission facts
};

// Outcome of cancellation.
struct CancelResult {
  bool cancelled = false;
  SequenceState state = SequenceState::Admitted;
  std::string reason;
};

// Outcome of a retry transition.
struct RetryResult {
  bool retried = false;
  AttemptId new_attempt;
  SequenceState state = SequenceState::Admitted;
  std::string reason;
};

// A formed and dispatched decode group: the unit of execution handed to a
// worker/executor. Carries the full authority token for the dispatch.
struct Dispatch {
  DispatchId id;
  DecodeGroupId group;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ReservationId reservation;
  CompatibilityKey key;
  DeviceDescriptor device;
  TimePoint issued_at;
  std::vector<DecodeMemberSpec> members;   // per-member spec (attempt/generation/state)

  std::string to_string() const;
};

// The Decode Fabric: owns iterative-decode scheduling, grouping, continuous
// batching, execution governance, cancellation, deadlines, budgets, fairness,
// reservations, memory accounting, observability, and persistence.
//
// Thread-safety: DecodeFabric is internally synchronized. All public methods
// may be called concurrently from multiple threads. No public method ever
// invokes a DecodeExecutor while holding the internal state lock, so callbacks
// cannot re-enter a held lock. Network/blocking I/O is never performed under
// the state lock.
class DecodeFabric {
 public:
  struct Config {
    // A clock reference. If null, a MonotonicClock is created and owned.
    Clock* clock = nullptr;
    GroupLimits group_limits;
    FabricLimits limits;
    RetryPolicy retry_policy;
    Persistence* persistence = nullptr;   // optional, borrowed
    std::uint32_t event_capacity = 16384;
    // If true, admission requires a registered worker that supports the model
    // key. If false, unknown-model requests are rejected anyway.
    bool require_known_model = true;
  };

  explicit DecodeFabric(Config config);
  ~DecodeFabric();
  DecodeFabric(DecodeFabric&&) noexcept;
  DecodeFabric& operator=(DecodeFabric&&) noexcept;
  DecodeFabric(const DecodeFabric&) = delete;
  DecodeFabric& operator=(const DecodeFabric&) = delete;

  // Admission. Creates a sequence for the request and places it in Waiting.
  AdmissionDecision submit(const DecodeRequest& req);

  // Request cancellation. Becomes authoritative at a safe step boundary.
  CancelResult cancel(SequenceId seq);
  CancelResult cancel_request(RequestId req);

  // Force a retry of a sequence (mint a new AttemptId).
  RetryResult retry(SequenceId seq);

  // Register/update a worker descriptor for placement.
  Result<void> register_worker(const WorkerDescriptor& wd);
  Result<void> mark_worker_dead(WorkerId id);
  Result<CoordinatorEpoch> roll_epoch();

  // Advance time-dependent state (deadlines, cancellations at boundaries).
  Result<void> advance(TimePoint now);

  // Form groups for the current ready set and emit dispatches. Does not execute.
  std::vector<Dispatch> schedule(TimePoint now);

  // Apply a completed execution group, validating all authority tokens and
  // advancing each sequence exactly once. Never advances stale/duplicate work.
  Result<void> apply_completion(const DecodeExecutionResult& result);

  // Transactional executor-state protocol (prepare/authorize/commit/receipt).
  struct GrantOrAbort {
    bool has_grant = false;
    CommitGrant grant;
    bool has_abort = false;
    AbortPrepared abort_spec;   // full authority tuple for the discard
    ProposalId proposal;        // the prepared proposal this entry refers to
    bool aborted = false;
    bool rejected = false;
    ErrorCode error_code = ErrorCode::Ok;
    std::string reason;
    MemberOutcome outcome;
  };
  struct AuthorizeResult {
    DispatchId dispatch_id;
    CoordinatorEpoch epoch;
    WorkerId worker;
    WorkerBootId worker_boot;
    std::vector<GrantOrAbort> members;
  };
  Result<AuthorizeResult> authorize_prepared(const PreparedDecode& prepared);
  Result<void> apply_commit_receipt(const ReceiptDecode& receipts);
  Result<void> abort_prepared(const AbortPrepared& abort);
  std::uint64_t sequence_committed_digest(SequenceId seq) const;
  bool has_pending_grant(SequenceId seq) const;
  GrantId pending_grant(SequenceId seq) const;
  int grant_status(GrantId grant) const;
  std::uint64_t receipt_count() const;

  // Synchronous convenience: one full pump (schedule -> execute -> complete).
  std::vector<Dispatch> pump_once(DecodeExecutor& executor, TimePoint now);
  // Run pumps until there is no runnable work (bounded by max_cycles; -1 = until idle).
  Result<void> pump_until_idle(DecodeExecutor& executor, TimePoint start, int max_cycles = -1);

  // Observability.
  Snapshot snapshot() const;
  Stats stats() const;
  std::vector<Event> events() const;
  Explain explain(SequenceId seq, const std::string& question) const;
  CoordinatorEpoch epoch() const;

  // Machine-readable & future-window helpers.
  std::uint32_t active_sequences() const;
  std::uint32_t ready_sequences() const;

  // Returns the in-flight dispatch authority for a sequence (for drivers that
  // need to preserve a pre-restart artifact for stale replay).
  struct DispatchAuthority {
    bool exists = false;
    DispatchId dispatch;
    CoordinatorEpoch epoch;
    WorkerId worker;
    WorkerBootId worker_boot;
    AttemptId attempt;
    DecodeGeneration generation;
    std::uint64_t generated = 0;
  };
  DispatchAuthority in_flight_authority(SequenceId seq) const;

  // Total stale-authority rejections recorded (for closure assertions).
  std::uint64_t stale_rejections() const;
  // Committed/total generated tokens for a sequence (0 if unknown).
  std::uint64_t sequence_generated(SequenceId seq) const;
  // Current state of a sequence (Admitted if unknown).
  SequenceState sequence_state(SequenceId seq) const;

  // Persistence: versioned, checksummed state snapshot + recovery.
  Result<std::vector<std::uint8_t>> serialize_state() const;
  Result<void> recover_state(const std::vector<std::uint8_t>& bytes);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace decodefabric
