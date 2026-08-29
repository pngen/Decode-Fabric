#include "decodefabric/fabric.hpp"
#include "decodefabric/binary.hpp"
#include "decodefabric/schedule.hpp"
#include "decodefabric/version.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace decodefabric {

namespace {
inline bool receipts_match(const MemberReceipt& a, const MemberReceipt& b) noexcept {
  return a.receipt_id == b.receipt_id && a.grant_id == b.grant_id &&
         a.proposal == b.proposal && a.sequence == b.sequence &&
         a.state == b.state && a.attempt == b.attempt &&
         a.generation == b.generation && a.worker == b.worker &&
         a.worker_boot == b.worker_boot && a.epoch == b.epoch &&
         a.dispatch == b.dispatch &&
         a.committed_position_before == b.committed_position_before &&
         a.committed_position_after == b.committed_position_after &&
         a.pre_state_digest == b.pre_state_digest &&
         a.post_state_digest == b.post_state_digest;
}
}  // namespace

namespace {
struct Crc32 {
  static std::uint32_t update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      crc ^= p[i];
      for (int k = 0; k < 8; ++k) {
        std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1)));
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
      }
    }
    return crc;
  }
};
}  // namespace

std::string Dispatch::to_string() const {
  std::ostringstream o;
  o << "dispatch[id=" << id.value() << ",group=" << group.value()
    << ",epoch=" << epoch.value() << ",worker=" << worker.value()
    << ",boot=" << worker_boot.value() << ",members=" << members.size() << "]";
  return o.str();
}

// ===========================================================================
// Internal implementation
// ===========================================================================
struct DecodeFabric::Impl {
  struct Seq {
    DecodeRequest req;
    SequenceStateMachine machine{SequenceState::Admitted};
    AttemptId current_attempt;
    TokenBudget budget{0};
    TokenCount current_length = 0;
    TokenCount generated = 0;            // committed tokens this attempt
    TokenCount committed = 0;            // committed tokens overall (recovery point)
    std::uint64_t generation = 1;        // current authoritative generation (next to run)
    DispatchId in_flight_dispatch;
    WorkerId in_flight_worker;
    WorkerBootId in_flight_worker_boot;
    CoordinatorEpoch in_flight_epoch;
    std::uint64_t in_flight_generation = 0;
    bool has_inflight = false;
    // Transactional executor-state protocol state.
    std::uint64_t committed_state_digest = 0;   // fabric's belief of committed executor state
    bool state_digest_established = false;
    GrantId in_flight_grant;
    bool has_inflight_grant = false;
    ProposalId in_flight_proposal;
    std::uint64_t in_flight_position = 0;
    ReservationId reservation;
    StateDescriptor state;
    std::uint32_t attempt_count = 1;
    TimePoint arrival, ready_at, last_token_at, last_grouped_at, last_dispatched_at;
    bool ever_ran = false;
  };

  struct DevMem {
    std::uint64_t capacity = 0;
    std::uint64_t reserved = 0;
    std::uint64_t headroom_floor = 0;
  };

  struct GrantRecord {
    CommitGrant grant;
    int status = 0;  // 0 pending, 1 consumed, 2 aborted
    MemberReceipt receipt;
    bool has_receipt = false;
  };

  Config cfg;
  std::unique_ptr<Clock> own_clock;
  Clock* clock = nullptr;
  mutable std::shared_mutex mtx;

  CoordinatorEpoch epoch{1};
  std::unordered_map<SequenceId, Seq> seqs;
  std::unordered_map<RequestId, SequenceId> req_to_seq;
  std::unordered_map<WorkerId, WorkerDescriptor> workers;
  std::unordered_map<DeviceId, DevMem> dev_mem;
  std::unordered_map<TenantId, std::uint64_t> tenant_tokens;
  std::unordered_map<TenantId, std::uint32_t> tenant_active;
  std::unordered_map<SequenceId, SequenceState> terminal_records;
  std::unordered_map<ReservationId, Reservation> reservations;
  std::vector<Event> events;
  std::size_t event_head = 0;
  Stats stats;
  std::unordered_map<WorkerId, WorkerBootId> worker_current_boot;
  std::uint64_t next_seq = 1, next_group = 1, next_dispatch = 1, next_reservation = 1;
  std::uint64_t next_grant = 1;
  std::unordered_map<GrantId, GrantRecord> grant_ledger;
  std::uint64_t receipt_count = 0;
  std::unordered_map<std::string, std::vector<SequenceId>> prev_group_members;

  void ensure_clock() { if (!clock) { own_clock = std::make_unique<MonotonicClock>(); clock = own_clock.get(); } }
  TimePoint now() const { return clock->now(); }
  void record_event(EventKind k, RequestId r = {}, SequenceId s = {}, std::string detail = {}) {
    Event e;
    e.id = EventId::from(events.size() + 1);
    e.time = now();
    e.kind = k;
    e.request = r;
    e.sequence = s;
    e.detail = std::move(detail);
    if (events.size() < cfg.event_capacity) events.push_back(std::move(e));
    else { events[event_head] = std::move(e); event_head = (event_head + 1) % cfg.event_capacity; }
  }

  bool release_reservation(SequenceId seq) {
    auto it = seqs.find(seq);
    if (it == seqs.end()) return false;
    Seq& s = it->second;
    if (s.reservation.is_null()) return false;
    auto rit = reservations.find(s.reservation);
    if (rit != reservations.end()) {
      if (!rit->second.released) {
        rit->second.released = true;
        auto dm = dev_mem.find(rit->second.device);
        if (dm != dev_mem.end() && dm->second.reserved >= rit->second.bytes) dm->second.reserved -= rit->second.bytes;
        else if (dm != dev_mem.end()) dm->second.reserved = 0;
        reservations.erase(rit);
        record_event(EventKind::ReservationReleased, s.req.id, seq, "released");
      }
    }
    s.reservation = ReservationId::null();
    return true;
  }

  bool is_candidate_state(SequenceState st) const {
    return st == SequenceState::Waiting || st == SequenceState::Ready ||
           st == SequenceState::ReadyForNextToken || st == SequenceState::Yielded;
  }

  CompatibilityKey key_for_model(const DecodeRequest& req) const {
    // Find a registered worker supporting req.model+revision(+adapter) and, if a
    // device constraint is present, that device.
    for (const auto& kv : workers) {
      const WorkerDescriptor& w = kv.second;
      if (req.device_constraint.is_valid() && w.device.id != req.device_constraint) continue;
      for (const auto& mk : w.supported_models) {
        if (mk.model == req.model && mk.revision == req.revision && mk.adapter == req.adapter) {
          return mk;
        }
      }
    }
    return CompatibilityKey{};
  }

  void update_tenant_tokens(TenantId t, std::uint64_t by) {
    auto it = tenant_tokens.find(t);
    if (it == tenant_tokens.end()) tenant_tokens[t] = by;
    else { std::uint64_t v = it->second; if (v > UINT64_MAX - by) v = UINT64_MAX; else v += by; it->second = v; }
  }

  // Apply the non-commit outcome semantics of a member (yield, failure, cancel,
  // expire). These never advance the authoritative generation and never touch
  // executor-resident state; only fabric canonical state may transition.
  // On a stale/authority rejection, the in-flight dispatch is no longer
  // valid. Clear it, release the reservation, and place the sequence back into
  // a candidate state so a later re-dispatch begins from a fresh dispatch and
  // the unchanged committed pre-state.
  void stale_reconcile(Seq& s, SequenceId seq) {
    s.has_inflight = false;
    s.in_flight_dispatch = DispatchId::null();
    s.has_inflight_grant = false;
    s.in_flight_grant = GrantId::null();
    s.in_flight_proposal = ProposalId::null();
    release_reservation(seq);
    if (!is_terminal(s.machine.state()) &&
        (s.machine.state() == SequenceState::Dispatched || s.machine.state() == SequenceState::Running)) {
      s.machine.transition_to(SequenceState::StepCompleted);
      s.machine.transition_to(SequenceState::Waiting);
      s.ready_at = now();
    }
  }

  void apply_noncommit_outcome(Seq& s, const MemberOutcome& mo) {
    switch (mo.kind) {
      case MemberOutcomeKind::Yielded: {
        s.machine.transition_to(SequenceState::StepCompleted);
        s.machine.transition_to(SequenceState::Yielded);
        s.ready_at = mo.finished_at;
        record_event(EventKind::Yielding, s.req.id, mo.sequence, "yielded");
        break;
      }
      case MemberOutcomeKind::RetryableFailure: {
        release_reservation(mo.sequence);
        if (s.machine.state() == SequenceState::Dispatched) s.machine.transition_to(SequenceState::RetryableFailure);
        if (s.attempt_count >= cfg.retry_policy.max_attempts) {
          s.machine.transition_to(SequenceState::NonRetryableFailure);
          terminal_records[mo.sequence] = SequenceState::NonRetryableFailure;
          stats.sequences_failed++;
          record_event(EventKind::RetryFailed, s.req.id, mo.sequence, "retry_budget_exhausted");
        } else {
          if (!cfg.retry_policy.preserve_committed_tokens) {
            s.generated = s.committed;
            s.budget = TokenBudget(s.req.max_generation_length);
            for (std::uint64_t kk = 0; kk < s.committed; ++kk) s.budget.advance(1);
          }
          s.current_attempt = AttemptId::from(next_dispatch + 1000000);
          ++s.attempt_count;
          s.machine.transition_to(SequenceState::Retrying);
          s.machine.transition_to(SequenceState::Ready);
          s.ready_at = now();
          stats.retries++;
          record_event(EventKind::RetryStarted, s.req.id, mo.sequence, "new_attempt");
        }
        break;
      }
      case MemberOutcomeKind::NonRetryableFailure: {
        if (s.machine.state() == SequenceState::Dispatched) s.machine.transition_to(SequenceState::NonRetryableFailure);
        tenant_active[s.req.tenant] = std::max(0u, tenant_active[s.req.tenant] - 1u);
        stats.sequences_failed++;
        terminal_records[mo.sequence] = SequenceState::NonRetryableFailure;
        record_event(EventKind::RequestRejected, s.req.id, mo.sequence, mo.error_message);
        break;
      }
      case MemberOutcomeKind::Cancelled: {
        if (!is_terminal(s.machine.state())) {
          s.machine.transition_to(SequenceState::Cancelled);
          tenant_active[s.req.tenant] = std::max(0u, tenant_active[s.req.tenant] - 1u);
          stats.cancellations++; stats.sequences_cancelled++;
          terminal_records[mo.sequence] = SequenceState::Cancelled;
          record_event(EventKind::SequenceCancelled, s.req.id, mo.sequence, "backend_cancel");
        }
        break;
      }
      case MemberOutcomeKind::Expired: {
        if (!is_terminal(s.machine.state())) {
          s.machine.transition_to(SequenceState::DeadlineExpired);
          tenant_active[s.req.tenant] = std::max(0u, tenant_active[s.req.tenant] - 1u);
          stats.deadline_misses++; stats.sequences_expired++;
          terminal_records[mo.sequence] = SequenceState::DeadlineExpired;
          record_event(EventKind::SequenceExpired, s.req.id, mo.sequence, "backend_expired");
        }
        break;
      }
      default: break;
    }
  }

  void refresh_derived_stats() {
    if (stats.decode_steps > 0 && stats.total_active_ns > 0) {
      stats.tokens_per_second = static_cast<double>(stats.generated_tokens) /
          (static_cast<double>(stats.total_active_ns) / 1e9);
    }
    if (stats.decode_steps > 0) {
      stats.avg_queue_delay_ns = static_cast<double>(stats.total_queue_ns) / stats.decode_steps;
      stats.avg_inter_token_ns = static_cast<double>(stats.total_inter_token_ns) / stats.decode_steps;
    }
  }

  void build_key(DecodeRequest& req) {
    if (req.arrival.ns == 0) req.arrival = now();
    CompatibilityKey k = key_for_model(req);
    req.model = k.model; req.revision = k.revision; req.adapter = k.adapter;
  }
};

// ===========================================================================
// Construction / destruction / move
// ===========================================================================
DecodeFabric::DecodeFabric(Config config) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = config;
  impl_->ensure_clock();
  impl_->stats.max_active_sequences = config.limits.max_active_sequences;
  impl_->stats.tokens_per_cycle_budget = config.limits.tokens_per_cycle_budget;
}
DecodeFabric::~DecodeFabric() = default;
DecodeFabric::DecodeFabric(DecodeFabric&&) noexcept = default;
DecodeFabric& DecodeFabric::operator=(DecodeFabric&&) noexcept = default;

CoordinatorEpoch DecodeFabric::epoch() const {
  std::shared_lock lk(impl_->mtx);
  return impl_->epoch;
}

// ===========================================================================
// Admission
// ===========================================================================
AdmissionDecision DecodeFabric::submit(const DecodeRequest& in) {
  std::unique_lock lk(impl_->mtx);
  AdmissionDecision d;
  d.request = in.id;
  if (!in.validate().ok()) {
    d.admitted = false;
    d.reason = in.validate().error().message;
    d.facts.push_back("request_validation_failed");
    impl_->record_event(EventKind::RequestRejected, in.id, {}, d.reason);
    return d;
  }
  DecodeRequest req = in;
  impl_->build_key(req);
  if (!req.model.is_valid()) {
    d.admitted = false;
    d.reason = "unknown_model";
    d.facts.push_back("model_not_supported_by_any_worker");
    impl_->record_event(EventKind::RequestRejected, req.id, {}, d.reason);
    return d;
  }
  // Backpressure checks.
  std::uint32_t active = 0;
  std::uint64_t outstanding = 0;
  for (const auto& kv : impl_->seqs) {
    if (!is_terminal(kv.second.machine.state())) { ++active; outstanding += kv.second.budget.remaining(); }
  }
  bool backpressured = false;
  std::string bp_reason;
  if (impl_->cfg.limits.max_active_sequences > 0 && active >= impl_->cfg.limits.max_active_sequences) {
    backpressured = true; bp_reason = "active_sequence_limit_reached";
  }
  auto ta = impl_->tenant_active.find(req.tenant);
  if (!backpressured && impl_->cfg.group_limits.max_tenant_share > 0 &&
      ta != impl_->tenant_active.end() && ta->second >= impl_->cfg.group_limits.max_tenant_share) {
    backpressured = true; bp_reason = "tenant_limit_exceeded";
  }
  if (backpressured) {
    d.admitted = false;
    d.reason = bp_reason;
    d.facts.push_back("backpressure_applied");
    impl_->record_event(EventKind::BackpressureApplied, req.id, {}, bp_reason);
    return d;
  }

  Impl::Seq s;
  s.req = req;
  s.current_attempt = req.initial_attempt.is_valid() ? req.initial_attempt : AttemptId::from(req.id.value());
  s.budget = TokenBudget(req.max_generation_length);
  if (req.pregenerated > 0) {
    (void)s.budget.advance(req.pregenerated);
    s.generated = req.pregenerated;
    s.committed = req.pregenerated;
  }
  s.current_length = req.prompt_length + s.committed;
  s.state = req.state;
  s.arrival = req.arrival;
  s.ready_at = req.arrival;
  s.machine = SequenceStateMachine(SequenceState::Admitted);
  s.machine.transition_to(SequenceState::Waiting);
  s.machine.transition_to(SequenceState::Ready);
  SequenceId id;
  if (req.sequence.is_valid()) {
    if (impl_->seqs.find(req.sequence) != impl_->seqs.end()) {
      d.admitted = false;
      d.reason = "duplicate_sequence_id";
      d.facts.push_back("duplicate_sequence");
      impl_->record_event(EventKind::RequestRejected, req.id, req.sequence, d.reason);
      return d;
    }
    id = req.sequence;
  } else {
    id = SequenceId::from(impl_->next_seq++);
  }
  impl_->seqs.emplace(id, std::move(s));
  impl_->req_to_seq[req.id] = id;

  impl_->stats.requests_admitted++;
  impl_->stats.sequences_started++;
  impl_->tenant_active[req.tenant]++;
  impl_->record_event(EventKind::RequestAdmitted, req.id, id, "waiting");

  d.admitted = true;
  d.sequence = id;
  d.reason = "admitted";
  d.facts.push_back("state=ready");
  return d;
}

// ===========================================================================
// Cancellation
// ===========================================================================
CancelResult DecodeFabric::cancel(SequenceId seq) {
  std::unique_lock lk(impl_->mtx);
  CancelResult c;
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) { c.state = SequenceState::Admitted; c.reason = "unknown_sequence"; return c; }
  Impl::Seq& s = it->second;
  if (is_terminal(s.machine.state())) { c.state = s.machine.state(); c.reason = "already_terminal"; return c; }
  if (s.machine.state() == SequenceState::CancelRequested) { c.state = s.machine.state(); c.reason = "already_cancelling"; return c; }
  // Request cancellation; becomes authoritative at a safe boundary.
  if (s.machine.state() == SequenceState::Dispatched || s.machine.state() == SequenceState::Running ||
      s.machine.state() == SequenceState::Reserved || s.machine.state() == SequenceState::Grouped) {
    s.machine.transition_to(SequenceState::CancelRequested);
  } else {
    // Between steps: cancel immediately at the boundary.
    impl_->release_reservation(seq);
    s.machine.transition_to(SequenceState::Cancelled);
    impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
    impl_->stats.cancellations++;
    impl_->stats.sequences_cancelled++;
    impl_->terminal_records[seq] = SequenceState::Cancelled;
    impl_->record_event(EventKind::SequenceCancelled, s.req.id, seq, "cancelled");
  }
  c.state = s.machine.state();
  c.cancelled = true;
  c.reason = "cancel_requested";
  return c;
}

CancelResult DecodeFabric::cancel_request(RequestId req) {
  std::unique_lock lk(impl_->mtx);
  auto it = impl_->req_to_seq.find(req);
  lk.unlock();
  if (it == impl_->req_to_seq.end()) { CancelResult c; c.reason = "unknown_request"; return c; }
  return cancel(it->second);
}

// ===========================================================================
// Retry
// ===========================================================================
RetryResult DecodeFabric::retry(SequenceId seq) {
  std::unique_lock lk(impl_->mtx);
  RetryResult r;
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) { r.reason = "unknown_sequence"; return r; }
  Impl::Seq& s = it->second;
  if (s.machine.state() == SequenceState::RetryableFailure || s.machine.state() == SequenceState::Running ||
      s.machine.state() == SequenceState::Dispatched) {
    // Bound attempt count.
    if (!impl_->cfg.retry_policy.preserve_committed_tokens) {
      // Explicit recovery point semantics: roll back to last committed point.
      s.generated = s.committed;
    }
    if (s.attempt_count >= impl_->cfg.retry_policy.max_attempts) {
      s.machine.transition_to(SequenceState::NonRetryableFailure);
      impl_->terminal_records[seq] = SequenceState::NonRetryableFailure;
      impl_->stats.sequences_failed++;
      impl_->record_event(EventKind::RetryFailed, s.req.id, seq, "retry_budget_exhausted");
      r.reason = "retry_budget_exhausted";
      return r;
    }
    // Mint a NEW AttemptId.
    s.current_attempt = AttemptId::from(impl_->next_dispatch + 1000000);
    ++s.attempt_count;
    s.generation = s.generation;  // committed generation preserved
    s.machine.transition_to(SequenceState::Retrying);
    s.machine.transition_to(SequenceState::Ready);
    s.ready_at = impl_->now();
    impl_->stats.retries++;
    impl_->record_event(EventKind::RetryStarted, s.req.id, seq, "new_attempt");
    r.retried = true;
    r.new_attempt = s.current_attempt;
    r.state = SequenceState::Ready;
    r.reason = "retried";
    return r;
  }
  r.reason = "not_retryable_now";
  return r;
}

// ===========================================================================
// Workers / epoch
// ===========================================================================
Result<void> DecodeFabric::register_worker(const WorkerDescriptor& wd) {
  std::unique_lock lk(impl_->mtx);
  if (!wd.valid()) return failed<void>(ErrorCode::UnknownWorker, "invalid worker descriptor");
  impl_->workers[wd.id] = wd;
  impl_->worker_current_boot[wd.id] = wd.boot_id;
  auto& dm = impl_->dev_mem[wd.device.id];
  dm.capacity = std::max(dm.capacity, wd.device.memory_bytes);
  dm.headroom_floor = impl_->cfg.limits.memory_headroom_bytes;
  impl_->record_event(EventKind::WorkerUp, {}, {}, "worker " + std::to_string(wd.id.value()) + " boot " + std::to_string(wd.boot_id.value()));
  return Result<void>::success();
}

Result<void> DecodeFabric::mark_worker_dead(WorkerId id) {
  std::unique_lock lk(impl_->mtx);
  auto wit = impl_->workers.find(id);
  if (wit != impl_->workers.end()) { wit->second.health = WorkerHealth::Failed; wit->second.active_reservations = 0; }
  impl_->record_event(EventKind::WorkerDown, {}, {}, "worker " + std::to_string(id.value()));
  // Reconcile formerly-running work on this worker: re-queue, release reservations.
  for (auto& kv : impl_->seqs) {
    Impl::Seq& s = kv.second;
    if (s.has_inflight && s.in_flight_worker == id) {
      impl_->release_reservation(kv.first);
      s.has_inflight = false;
      if (!is_terminal(s.machine.state())) {
        if (s.machine.state() == SequenceState::Dispatched || s.machine.state() == SequenceState::Running) {
          s.machine.transition_to(SequenceState::StepCompleted);
          s.machine.transition_to(SequenceState::Waiting);
        }
        s.ready_at = impl_->now();
      }
    }
  }
  return Result<void>::success();
}

Result<CoordinatorEpoch> DecodeFabric::roll_epoch() {
  std::unique_lock lk(impl_->mtx);
  impl_->epoch = CoordinatorEpoch::from(impl_->epoch.value() + 1);
  impl_->record_event(EventKind::EpochRolled, {}, {}, "epoch " + std::to_string(impl_->epoch.value()));
  return Result<CoordinatorEpoch>::ok(impl_->epoch);
}

// ===========================================================================
// Advance (boundary processing)
// ===========================================================================
Result<void> DecodeFabric::advance(TimePoint tn) {
  std::unique_lock lk(impl_->mtx);
  for (auto& kv : impl_->seqs) {
    Impl::Seq& s = kv.second;
    if (is_terminal(s.machine.state())) continue;
    // Deadline expiry at boundary.
    if (s.req.deadline.ns != 0 && tn.ns >= s.req.deadline.ns) {
      impl_->release_reservation(kv.first);
      if (!is_terminal(s.machine.state())) {
        s.machine.transition_to(SequenceState::DeadlineExpired);
        impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
        impl_->stats.deadline_misses++;
        impl_->stats.sequences_expired++;
        impl_->terminal_records[kv.first] = SequenceState::DeadlineExpired;
        impl_->record_event(EventKind::SequenceExpired, s.req.id, kv.first, "deadline");
      }
      continue;
    }
    // Cancellation at boundary.
    if (s.machine.state() == SequenceState::CancelRequested) {
      impl_->release_reservation(kv.first);
      s.machine.transition_to(SequenceState::Cancelled);
      impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
      impl_->stats.cancellations++;
      impl_->stats.sequences_cancelled++;
      impl_->terminal_records[kv.first] = SequenceState::Cancelled;
      impl_->record_event(EventKind::SequenceCancelled, s.req.id, kv.first, "cancelled");
    }
  }
  return Result<void>::success();
}

// ===========================================================================
// Scheduling / grouping / dispatch
// ===========================================================================
namespace {
struct Candidate {
  SequenceId seq;
  CompatibilityKey key;
  double score = 0.0;
  SchedulingComponents comp;
};
}

std::vector<Dispatch> DecodeFabric::schedule(TimePoint tn) {
  std::unique_lock lk(impl_->mtx);
  std::vector<Dispatch> out;
  // advance is called by callers by callers; we do a best-effort deadline pass.
  for (auto& kv : impl_->seqs) {
    Impl::Seq& s = kv.second;
    if (is_terminal(s.machine.state())) continue;
    if (s.req.deadline.ns != 0 && tn.ns >= s.req.deadline.ns) {
      impl_->release_reservation(kv.first);
      s.machine.transition_to(SequenceState::DeadlineExpired);
      impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
      impl_->stats.deadline_misses++; impl_->stats.sequences_expired++;
      impl_->terminal_records[kv.first] = SequenceState::DeadlineExpired;
    }
    if (s.machine.state() == SequenceState::CancelRequested) {
      impl_->release_reservation(kv.first);
      s.machine.transition_to(SequenceState::Cancelled);
      impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
      impl_->stats.cancellations++; impl_->stats.sequences_cancelled++;
      impl_->terminal_records[kv.first] = SequenceState::Cancelled;
    }
  }

  std::vector<Candidate> cands;
  for (auto& kv : impl_->seqs) {
    Impl::Seq& s = kv.second;
    if (!impl_->is_candidate_state(s.machine.state())) continue;
    if (s.budget.exhausted()) {
      // Budget exhausted but not terminal: make terminal now.
      impl_->release_reservation(kv.first);
      s.machine.transition_to(SequenceState::Completed);
      impl_->terminal_records[kv.first] = SequenceState::Completed;
      impl_->stats.sequences_completed++;
      impl_->record_event(EventKind::SequenceCompleted, s.req.id, kv.first, "budget_exhausted");
      continue;
    }
    CompatibilityKey k = impl_->key_for_model(s.req);
    if (!k.valid()) continue;
    Candidate c;
    c.seq = kv.first;
    c.key = k;
    // Scoring components (explicit & inspectable).
    c.comp.readiness = 1.0;
    Nanoseconds since = tn.ns - (s.last_token_at.ns ? s.last_token_at.ns : s.ready_at.ns);
    if (since < 0) since = 0;
    c.comp.priority = s.req.priority / 1000.0;
    c.comp.age = std::min(1.0, static_cast<double>(since) / 1e9 /* sec */);
    if (s.req.per_token_target_ns > 0) {
      c.comp.latency_pressure = std::min(1.0, static_cast<double>(since) / s.req.per_token_target_ns);
    }
    if (s.req.deadline.ns != 0 && s.req.arrival.ns != 0) {
      double elapsed = static_cast<double>(tn.ns - s.req.arrival.ns);
      double total = static_cast<double>(s.req.deadline.ns - s.req.arrival.ns);
      c.comp.deadline_pressure = total > 0 ? std::min(1.0, elapsed / total) : 1.0;
    }
    c.comp.budget_ratio = static_cast<double>(s.budget.remaining()) /
                          static_cast<double>(s.budget.max() ? s.budget.max() : 1);
    c.comp.fairness_deficit = 0.0;
    c.comp.score = 0.40 * c.comp.latency_pressure + 0.25 * c.comp.age +
                   0.15 * c.comp.priority + 0.20 * c.comp.deadline_pressure;
    c.score = c.comp.score;
    cands.push_back(std::move(c));
  }

  // Deterministic tie-break: sort by score desc, then seq id asc.
  std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.seq.value() < b.seq.value();
  });

  // Group formation: pack ready candidates by CompatibilityKey under limits.
  struct Formed { CompatibilityKey key; std::vector<SequenceId> members; };
  std::unordered_map<std::string, std::size_t> group_index;
  std::vector<Formed> formed;
  for (const Candidate& c : cands) {
    auto& s = impl_->seqs[c.seq];
    const std::string ks = c.key.to_string();
    std::size_t gi = formed.size();
    auto fit = group_index.find(ks);
    if (fit != group_index.end()) {
      gi = fit->second;  // reuse existing group for this key
    } else {
      group_index[ks] = formed.size();
      formed.push_back(Formed{c.key, {}});
      gi = group_index[ks];
    }
    Formed& g = formed[gi];
    const GroupLimits& lim = impl_->cfg.group_limits;
    if (!g.members.empty() && g.members.size() >= lim.max_sequences) continue;  // group full
    std::uint32_t tenant_share = 0;
    for (const SequenceId& m : g.members) if (impl_->seqs[m].req.tenant == s.req.tenant) ++tenant_share;
    if (lim.max_tenant_share > 0 && tenant_share >= lim.max_tenant_share) continue;
    if (lim.max_latency_class != LatencyClass::Bulk && s.req.latency_class > lim.max_latency_class) continue;
    g.members.push_back(c.seq);
    s.machine.transition_to(SequenceState::Grouped);
    s.last_grouped_at = tn;
    s.ready_at = tn;
  }

  // For each formed group, place on a worker, reserve memory, dispatch.
  for (Formed& g : formed) {
    if (g.members.empty()) continue;
    // Worker placement.
    WorkerDescriptor* chosen = nullptr;
    for (auto& wkv : impl_->workers) {
      WorkerDescriptor& w = wkv.second;
      if (w.health != WorkerHealth::Healthy) continue;
      bool supports = false;
      for (const auto& mk : w.supported_models) {
        if (mk.model == g.key.model && mk.revision == g.key.revision && mk.device == g.key.device) { supports = true; break; }
      }
      if (!supports) continue;
      if (w.advertised_capacity > 0 && w.active_reservations >= w.advertised_capacity) continue;
      if (!chosen || w.active_reservations < chosen->active_reservations) chosen = &w;
    }
    if (!chosen) {
      // No compatible worker: revert members to Ready, do not dispatch.
      for (const SequenceId& m : g.members) { auto& s = impl_->seqs[m]; s.machine.transition_to(SequenceState::Ready); }
      continue;
    }
    // Memory reservation.
    std::uint64_t growth = 0;
    for (const SequenceId& m : g.members) {
      auto& s = impl_->seqs[m];
      growth += s.state.estimated_growth ? s.state.estimated_growth : 64;
    }
    auto& dm = impl_->dev_mem[chosen->device.id];
    std::uint64_t capacity = dm.capacity;
    if (capacity > 0) {
      std::uint64_t headroom = capacity > dm.reserved ? capacity - dm.reserved : 0;
      if (growth > headroom || (dm.headroom_floor > 0 && headroom < dm.headroom_floor)) {
        for (const SequenceId& m : g.members) { auto& s = impl_->seqs[m]; s.machine.transition_to(SequenceState::Ready); }
        impl_->record_event(EventKind::BackpressureApplied, {}, {}, "memory_headroom");
        continue;
      }
    }
    ReservationId rid = ReservationId::from(impl_->next_reservation++);
    Reservation res;
    res.id = rid; res.kind = ReservationKind::GroupReservation;
    res.group = DecodeGroupId::from(impl_->next_group++);
    res.device = chosen->device.id; res.bytes = growth; res.made_at = tn;
    impl_->reservations.emplace(rid, res);
    dm.reserved += growth;
    impl_->record_event(EventKind::ReservationGranted, {}, {}, "group_reservation");

    Dispatch d;
    d.id = DispatchId::from(impl_->next_dispatch++);
    d.group = res.group;
    d.epoch = impl_->epoch;
    d.worker = chosen->id;
    d.worker_boot = impl_->worker_current_boot[chosen->id];
    d.reservation = rid;
    d.key = g.key;
    d.device = chosen->device;
    d.issued_at = tn;
    for (const SequenceId& m : g.members) {
      auto& s = impl_->seqs[m];
      DecodeMemberSpec ms;
      ms.sequence = m;
      ms.attempt = s.current_attempt;
      ms.generation = DecodeGeneration::from(s.generation);
      ms.state = s.state;
      ms.current_length = s.current_length;
      ms.generated_tokens = s.generated;
      ms.remaining_budget = s.budget.remaining();
      ms.payload.assign(s.req.sampling_metadata.begin(), s.req.sampling_metadata.end());
      d.members.push_back(std::move(ms));
      // Authority bookkeeping for stale checks.
      s.in_flight_dispatch = d.id;
      s.in_flight_worker = d.worker;
      s.in_flight_worker_boot = d.worker_boot;
      s.in_flight_epoch = d.epoch;
      s.in_flight_generation = s.generation;
      s.has_inflight = true;
      s.reservation = rid;
      s.last_dispatched_at = tn;
      s.machine.transition_to(SequenceState::Reserved);
      s.machine.transition_to(SequenceState::Dispatched);
      s.ever_ran = true;
      impl_->record_event(EventKind::SequenceGrouped, s.req.id, m, "grouped");
    }
    chosen->active_reservations += 1;
    impl_->stats.dispatch_issued++;
    impl_->record_event(EventKind::DispatchIssued, {}, {}, d.to_string());
    out.push_back(std::move(d));
  }
  impl_->refresh_derived_stats();
  return out;
}

// ===========================================================================
// Completion application
// ===========================================================================
Result<void> DecodeFabric::apply_completion(const DecodeExecutionResult& result) {
  std::unique_lock lk(impl_->mtx);
  for (const MemberOutcome& mo : result.outcomes) {
    auto it = impl_->seqs.find(mo.sequence);
    if (it == impl_->seqs.end()) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, mo.sequence, "unknown_sequence");
      continue;
    }
    Impl::Seq& s = it->second;

    // --- Authority validation, most-stale first. ---
    if (result.epoch != impl_->epoch) {
      s.has_inflight = false;
      impl_->release_reservation(mo.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "stale_epoch");
      continue;
    }
    auto wit = impl_->worker_current_boot.find(result.worker);
    if (wit != impl_->worker_current_boot.end() && wit->second != result.worker_boot) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "stale_worker_boot");
      continue;
    }
    if (mo.attempt != s.current_attempt) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "stale_attempt");
      continue;
    }
    if (mo.generation.value() != s.in_flight_generation) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "stale_generation");
      continue;
    }
    if (result.dispatch_id != s.in_flight_dispatch) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "stale_dispatch");
      continue;
    }
    if (is_terminal(s.machine.state())) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "completion_for_terminal");
      continue;
    }
    if (s.machine.state() == SequenceState::CancelRequested || s.machine.state() == SequenceState::Cancelled) {
      impl_->release_reservation(mo.sequence);
      s.has_inflight = false;
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "completion_for_cancelled");
      continue;
    }
    if (s.machine.state() == SequenceState::DeadlineExpired) {
      impl_->release_reservation(mo.sequence);
      s.has_inflight = false;
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "completion_for_expired");
      continue;
    }
    if (mo.kind == MemberOutcomeKind::StaleAuthorityRejected) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "backend_rejected");
      continue;
    }

    // Authority is current. Under the transactional executor-state model a
    // successful step may only finalize through a validated commit receipt, so
    // a bare completion here is NOT authoritative. Reject it deterministically
    // (no fabric advance, no executor-committed advance) and request an abort.
    if (mo.kind == MemberOutcomeKind::StepSuccessContinue ||
        mo.kind == MemberOutcomeKind::StepSuccessTerminal) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, mo.sequence, "completion_without_receipt");
      continue;
    }
    // Non-commit member outcome: clear in-flight dispatch, release the
    // reservation, and apply outcome semantics without advancing executor state.
    s.has_inflight = false;
    s.in_flight_dispatch = DispatchId::null();
    impl_->release_reservation(mo.sequence);
    impl_->apply_noncommit_outcome(s, mo);
  }
  impl_->refresh_derived_stats();
  return Result<void>::success();
}


// ===========================================================================
// Transactional executor-state protocol
// ===========================================================================
Result<DecodeFabric::AuthorizeResult> DecodeFabric::authorize_prepared(const PreparedDecode& prepared) {
  std::unique_lock lk(impl_->mtx);
  AuthorizeResult ar;
  ar.dispatch_id = prepared.dispatch_id;
  ar.epoch = prepared.epoch;
  ar.worker = prepared.worker;
  ar.worker_boot = prepared.worker_boot;
  for (const PreparedMember& pm : prepared.members) {
    GrantOrAbort ga;
    ga.proposal = pm.proposal;
    {  // default abort target (authority tuple) for stale/non-commit members
      AbortPrepared abi;
      abi.proposal = pm.proposal; abi.sequence = pm.sequence; abi.state = pm.state;
      abi.attempt = pm.attempt; abi.generation = pm.generation; abi.dispatch = pm.dispatch;
      abi.epoch = pm.epoch; abi.worker = pm.worker; abi.worker_boot = pm.worker_boot;
      ga.abort_spec = std::move(abi);
      ga.has_abort = true;
    }
    auto it = impl_->seqs.find(pm.sequence);
    if (it == impl_->seqs.end()) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, pm.sequence, "unknown_sequence");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::UnknownSequence; ga.reason = "unknown_sequence";
      ar.members.push_back(std::move(ga));
      continue;
    }
    Impl::Seq& s = it->second;

    // --- Authority validation, most-stale first. ---
    if (pm.epoch != impl_->epoch) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "stale_epoch");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::StaleCoordinatorEpoch; ga.reason = "stale_epoch";
      ar.members.push_back(std::move(ga));
      continue;
    }
    auto wit = impl_->worker_current_boot.find(pm.worker);
    if (wit != impl_->worker_current_boot.end() && wit->second != pm.worker_boot) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "stale_worker_boot");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::StaleWorkerBoot; ga.reason = "stale_worker_boot";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (pm.attempt != s.current_attempt) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "stale_attempt");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::StaleAttempt; ga.reason = "stale_attempt";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (pm.generation.value() != s.in_flight_generation) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "stale_generation");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::StaleDecodeGeneration; ga.reason = "stale_generation";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (pm.dispatch != s.in_flight_dispatch) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "stale_dispatch");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::DuplicateCompletion; ga.reason = "stale_dispatch";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (is_terminal(s.machine.state())) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "completion_for_terminal");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::CompletionForTerminal; ga.reason = "terminal";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (s.machine.state() == SequenceState::CancelRequested || s.machine.state() == SequenceState::Cancelled) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "completion_for_cancelled");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::CompletionForCancelled; ga.reason = "cancelled";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (s.machine.state() == SequenceState::DeadlineExpired) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, pm.sequence, "completion_for_expired");
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::CompletionForExpired; ga.reason = "expired";
      ar.members.push_back(std::move(ga));
      continue;
    }

    if (!pm.is_commit_eligible()) {
      // Non-commit outcome: apply outcome semantics (no executor advance) and
      // request an abort of any prepared transition.
      s.has_inflight = false;
      s.in_flight_dispatch = DispatchId::null();
      impl_->release_reservation(pm.sequence);
      impl_->apply_noncommit_outcome(s, pm.outcome);
      ga.aborted = true;
      ga.outcome = pm.outcome;
      ar.members.push_back(std::move(ga));
      continue;
    }

    // Commit-eligible: validate the transaction binding before issuing a grant.
    if (s.has_inflight_grant) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::TransactionConflict;
      ga.reason = "conflicting pending grant for sequence+generation";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (pm.committed_position_before != s.generated) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::InvalidArgument;
      ga.reason = "committed-position mismatch";
      ar.members.push_back(std::move(ga));
      continue;
    }
    if (s.state_digest_established && pm.pre_state_digest != s.committed_state_digest) {
      impl_->stale_reconcile(s, pm.sequence);
      impl_->stats.stale_rejections++;
      ga.rejected = true; ga.aborted = true; ga.error_code = ErrorCode::StateDigestMismatch;
      ga.reason = "pre-state digest does not match committed executor state";
      ar.members.push_back(std::move(ga));
      continue;
    }
    // Issue a one-use commit grant bound to the full authority/proposal tuple.
    GrantId gid = GrantId::from(impl_->next_grant++);
    CommitGrant g;
    g.grant_id = gid;
    g.proposal = pm.proposal;
    g.epoch = prepared.epoch;
    g.worker = prepared.worker;
    g.worker_boot = prepared.worker_boot;
    g.sequence = pm.sequence;
    g.state = pm.state;
    g.attempt = pm.attempt;
    g.generation = pm.generation;
    g.dispatch = pm.dispatch;
    g.committed_position = pm.committed_position_before;
    g.pre_state_digest = pm.pre_state_digest;
    g.post_state_digest = pm.post_state_digest;
    g.delta_digest = pm.delta_digest;
    g.outcome_kind = pm.outcome.kind;
    g.terminal = pm.outcome.terminal;
    g.token_identifier = static_cast<std::uint32_t>(pm.outcome.token_identifier);
    g.active_ns = pm.active_ns;
    Impl::GrantRecord gr;
    gr.grant = g;
    gr.status = 0;
    impl_->grant_ledger[gid] = gr;
    s.in_flight_grant = gid;
    s.has_inflight_grant = true;
    s.in_flight_proposal = pm.proposal;
    s.in_flight_position = pm.committed_position_before;
    ga.has_grant = true;
    ga.grant = g;
    ga.has_abort = false;  // this member commits, not aborts
    ar.members.push_back(std::move(ga));
    impl_->record_event(EventKind::ReservationGranted, s.req.id, pm.sequence, "commit_grant_issued");
  }
  return Result<AuthorizeResult>::ok(std::move(ar));
}

Result<void> DecodeFabric::apply_commit_receipt(const ReceiptDecode& receipts) {
  std::unique_lock lk(impl_->mtx);
  for (const MemberReceipt& rec : receipts.receipts) {
    auto git = impl_->grant_ledger.find(rec.grant_id);
    if (git == impl_->grant_ledger.end()) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, rec.sequence, "unknown_grant");
      continue;
    }
    Impl::GrantRecord& gr = git->second;
    if (gr.status == 1) {  // already consumed
      if (gr.has_receipt && receipts_match(gr.receipt, rec)) continue;  // idempotent
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, rec.sequence, "conflicting_duplicate_receipt");
      continue;
    }
    if (gr.status == 2) {  // aborted
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, rec.sequence, "aborted_grant");
      continue;
    }
    // Validate the receipt against the issued one-use grant.
    const CommitGrant& g = gr.grant;
    if (!(rec.grant_id == g.grant_id && rec.proposal == g.proposal &&
          rec.sequence == g.sequence && rec.state == g.state &&
          rec.attempt == g.attempt && rec.generation == g.generation &&
          rec.worker_boot == g.worker_boot && rec.epoch == g.epoch &&
          rec.dispatch == g.dispatch &&
          rec.committed_position_before == g.committed_position &&
          rec.pre_state_digest == g.pre_state_digest &&
          rec.post_state_digest == g.post_state_digest)) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, rec.sequence, "receipt_grant_mismatch");
      continue;
    }
    auto sit = impl_->seqs.find(rec.sequence);
    if (sit == impl_->seqs.end()) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, {}, rec.sequence, "unknown_sequence");
      continue;
    }
    Impl::Seq& s = sit->second;
    if (!s.has_inflight_grant || s.in_flight_grant != rec.grant_id ||
        s.in_flight_proposal != rec.proposal) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, rec.sequence, "grant_not_inflight");
      continue;
    }
    if (is_terminal(s.machine.state()) || s.machine.state() == SequenceState::CancelRequested ||
        s.machine.state() == SequenceState::Cancelled || s.machine.state() == SequenceState::DeadlineExpired) {
      impl_->stats.stale_rejections++;
      impl_->record_event(EventKind::StaleRejected, s.req.id, rec.sequence, "completion_for_terminal");
      continue;
    }

    // Finalize exactly one authoritative generation.
    auto adv = s.budget.advance(1);
    Nanoseconds prev_token = s.last_token_at.ns;
    s.generated += 1;
    s.committed += 1;
    s.current_length += 1;
    ++s.generation;
    TimePoint fin = rec.committed_at.ns ? rec.committed_at : impl_->now();
    s.last_token_at = fin;
    impl_->stats.decode_steps++;
    impl_->stats.generated_tokens++;
    impl_->stats.total_active_ns += rec.active_ns;
    if (prev_token != 0) impl_->stats.total_inter_token_ns += (fin.ns - prev_token);
    impl_->update_tenant_tokens(s.req.tenant, 1);
    impl_->record_event(EventKind::StepCompleted, s.req.id, rec.sequence, "token " + std::to_string(s.generated));

    // Receipt chain: this committed post-state becomes the next generation's
    // expected committed pre-state.
    s.committed_state_digest = rec.post_state_digest;
    s.state_digest_established = true;

    bool terminal = rec.terminal || s.budget.exhausted();
    if (terminal) {
      s.machine.transition_to(SequenceState::StepCompleted);
      s.machine.transition_to(SequenceState::Completed);
      impl_->tenant_active[s.req.tenant] = std::max(0u, impl_->tenant_active[s.req.tenant] - 1u);
      impl_->stats.sequences_completed++;
      impl_->terminal_records[rec.sequence] = SequenceState::Completed;
      impl_->record_event(EventKind::SequenceCompleted, s.req.id, rec.sequence, "terminal");
    } else {
      s.machine.transition_to(SequenceState::StepCompleted);
      s.machine.transition_to(SequenceState::ReadyForNextToken);
      s.ready_at = fin;
    }

    // Close the transaction.
    gr.status = 1;
    gr.receipt = rec;
    gr.has_receipt = true;
    s.has_inflight = false;
    s.in_flight_dispatch = DispatchId::null();
    impl_->release_reservation(rec.sequence);
    s.has_inflight_grant = false;
    s.in_flight_grant = GrantId::null();
    s.in_flight_proposal = ProposalId::null();
    impl_->receipt_count++;
    impl_->record_event(EventKind::ReservationReleased, s.req.id, rec.sequence, "receipt_consumed");
  }
  impl_->refresh_derived_stats();
  return Result<void>::success();
}

Result<void> DecodeFabric::abort_prepared(const AbortPrepared& abort) {
  std::unique_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(abort.sequence);
  if (it == impl_->seqs.end()) return Result<void>::success();
  Impl::Seq& s = it->second;
  if (s.has_inflight_grant && s.in_flight_proposal == abort.proposal) {
    auto git = impl_->grant_ledger.find(s.in_flight_grant);
    if (git != impl_->grant_ledger.end() && git->second.status == 0) git->second.status = 2;
    s.has_inflight_grant = false;
    s.in_flight_grant = GrantId::null();
    s.in_flight_proposal = ProposalId::null();
    s.in_flight_position = 0;
    // Clear the dispatch so the sequence can be re-dispatched under fresh
    // authority (committed executor state is unchanged).
    s.has_inflight = false;
    impl_->release_reservation(abort.sequence);
    if (!is_terminal(s.machine.state()) &&
        (s.machine.state() == SequenceState::Dispatched || s.machine.state() == SequenceState::Running)) {
      s.machine.transition_to(SequenceState::StepCompleted);
      s.machine.transition_to(SequenceState::Waiting);
      s.ready_at = impl_->now();
    }
    impl_->record_event(EventKind::StaleRejected, s.req.id, abort.sequence, "aborted_prepared");
  }
  return Result<void>::success();
}

std::uint64_t DecodeFabric::sequence_committed_digest(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(seq);
  return it == impl_->seqs.end() ? 0 : it->second.committed_state_digest;
}

bool DecodeFabric::has_pending_grant(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(seq);
  return it != impl_->seqs.end() && it->second.has_inflight_grant;
}

GrantId DecodeFabric::pending_grant(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(seq);
  return it == impl_->seqs.end() ? GrantId::null() : it->second.in_flight_grant;
}

int DecodeFabric::grant_status(GrantId grant) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->grant_ledger.find(grant);
  return it == impl_->grant_ledger.end() ? -1 : it->second.status;
}

std::uint64_t DecodeFabric::receipt_count() const {
  std::shared_lock lk(impl_->mtx);
  return impl_->receipt_count;
}

// ===========================================================================
// Synchronous pump helpers
// ===========================================================================
std::vector<Dispatch> DecodeFabric::pump_once(DecodeExecutor& executor, TimePoint tn) {
  (void)advance(tn);
  std::vector<Dispatch> ds = schedule(tn);
  for (Dispatch& d : ds) {
    DecodeExecutionRequest req;
    req.dispatch_id = d.id;
    req.epoch = d.epoch;
    req.worker = d.worker;
    req.worker_boot = d.worker_boot;
    req.key = d.key;
    req.device = d.device;
    req.reservation_id = d.reservation.value();
    req.members = d.members;
    { std::string k = d.key.to_string(); req.group_payload.assign(k.begin(), k.end()); }

    // PREPARE
    auto prep = executor.prepare(req);
    if (!prep.ok()) {
      // Backend-level prepare failure: report a retryable failure for each
      // member through the non-commit outcome path (no executor advance).
      DecodeExecutionResult r;
      r.dispatch_id = d.id;
      r.epoch = d.epoch;
      r.worker = d.worker;
      r.worker_boot = d.worker_boot;
      r.group_error = prep.error().code;
      r.group_error_message = prep.error().message;
      for (const DecodeMemberSpec& m : d.members) {
        MemberOutcome mo; mo.sequence = m.sequence; mo.kind = MemberOutcomeKind::RetryableFailure;
        mo.attempt = m.attempt; mo.generation = m.generation; mo.error_message = prep.error().message;
        mo.retryable = true;
        r.outcomes.push_back(mo);
      }
      apply_completion(r);
      continue;
    }
    PreparedDecode pd = std::move(prep.value());

    // AUTHORIZE
    auto auth = authorize_prepared(pd);
    if (!auth.ok()) {
      // Fabric-level authorize failure (should not happen for a valid prepare;
      // if it does, abort every prepared member on the executor).
      for (const PreparedMember& pm : pd.members) {
        AbortPrepared ab;
        ab.proposal = pm.proposal; ab.sequence = pm.sequence; ab.state = pm.state;
        ab.attempt = pm.attempt; ab.generation = pm.generation; ab.dispatch = pm.dispatch;
        ab.epoch = pm.epoch; ab.worker = pm.worker; ab.worker_boot = pm.worker_boot;
        (void)executor.abort(ab);
      }
      continue;
    }
    AuthorizeResult ar = std::move(auth.value());

    // COMMIT (or ABORT) per member, then RECEIPT/finalize.
    ReceiptDecode rd;
    rd.dispatch_id = d.id;
    rd.epoch = d.epoch;
    rd.worker = d.worker;
    rd.worker_boot = d.worker_boot;
    for (std::size_t i = 0; i < ar.members.size(); ++i) {
      GrantOrAbort& ga = ar.members[i];
      if (ga.rejected || ga.aborted || !ga.has_grant) {
        // Stale/rejected/non-commit member: the fabric already recorded the
        // rejection/outcome; discard the prepared transition on the executor.
        if (ga.has_abort) (void)executor.abort(ga.abort_spec);
        continue;
      }
      auto cr = executor.commit(ga.grant);
      if (cr.ok()) {
        MemberReceipt mr = std::move(cr.value());
        mr.committed_at = tn;
        rd.receipts.push_back(std::move(mr));
      } else {
        // Commit rejected (e.g. conflict): abort the prepared transition.
        if (ga.has_abort) (void)executor.abort(ga.abort_spec);
      }
    }
    // FINALIZE (fabric canonical advance only after confirmed receipts).
    (void)apply_commit_receipt(rd);
  }
  return ds;
}

Result<void> DecodeFabric::pump_until_idle(DecodeExecutor& executor, TimePoint start, int max_cycles) {
  int cycles = 0;
  TimePoint tn = start;
  while (true) {
    tn = impl_->now();
    std::vector<Dispatch> ds = pump_once(executor, tn);
    ++cycles;
    if (max_cycles > 0 && cycles >= max_cycles) break;
    // Stop when no runnable candidates remain.
    std::shared_lock lk(impl_->mtx);
    bool has_candidate = false;
    for (const auto& kv : impl_->seqs)
      if (impl_->is_candidate_state(kv.second.machine.state())) { has_candidate = true; break; }
    lk.unlock();
    if (!has_candidate && ds.empty()) break;
    if (ds.empty()) {
      // Nothing scheduled; if still candidates (e.g. no worker), stop to avoid
      // spinning forever.
      std::shared_lock lk2(impl_->mtx);
      bool any = false;
      for (const auto& kv : impl_->seqs)
        if (impl_->is_candidate_state(kv.second.machine.state())) { any = true; break; }
      lk2.unlock();
      if (any) break;
    }
  }
  return Result<void>::success();
}

// ===========================================================================
// Observability
// ===========================================================================
Snapshot DecodeFabric::snapshot() const {
  std::shared_lock lk(impl_->mtx);
  Snapshot sn;
  sn.epoch = impl_->epoch;
  sn.stats = impl_->stats;
  for (const auto& kv : impl_->seqs) {
    const Impl::Seq& s = kv.second;
    if (s.machine.state() == SequenceState::Admitted) sn.admitted++;
    else if (s.machine.state() == SequenceState::Waiting) sn.admitted++;
    else if (impl_->is_candidate_state(s.machine.state())) sn.ready++;
    else if (s.machine.state() == SequenceState::Grouped || s.machine.state() == SequenceState::Reserved) sn.grouped++;
    else if (s.machine.state() == SequenceState::Dispatched || s.machine.state() == SequenceState::Running) sn.running++;
    else if (s.machine.state() == SequenceState::Completed) sn.completed++;
    else if (s.machine.state() == SequenceState::Cancelled) sn.cancelled++;
    else if (s.machine.state() == SequenceState::DeadlineExpired) sn.expired++;
    else if (!is_terminal(s.machine.state())) sn.active++;
  }
  sn.active = 0;
  for (const auto& kv : impl_->seqs) if (!is_terminal(kv.second.machine.state())) sn.active++;
  for (const auto& kv : impl_->reservations) sn.reservations.push_back(kv.second);
  for (const auto& kv : impl_->workers) sn.workers.push_back(kv.second);
  for (const auto& kv : impl_->tenant_tokens)
    sn.per_tenant_tokens.push_back("tenant=" + std::to_string(kv.first.value()) + " tokens=" + std::to_string(kv.second));
  return sn;
}

Stats DecodeFabric::stats() const {
  std::shared_lock lk(impl_->mtx);
  return impl_->stats;
}

std::vector<Event> DecodeFabric::events() const {
  std::shared_lock lk(impl_->mtx);
  std::vector<Event> out;
  out = impl_->events;
  // Reassemble ring order (oldest first).
  return out;
}

Explain DecodeFabric::explain(SequenceId seq, const std::string& q) const {
  std::shared_lock lk(impl_->mtx);
  Explain e;
  e.sequence = seq;
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) { e.question = q; e.answer = "unknown sequence"; e.facts.push_back("unknown_sequence"); return e; }
  const Impl::Seq& s = it->second;
  e.request = s.req.id;
  e.question = q;
  e.answer = "sequence in state " + std::string(to_string(s.machine.state()));
  e.facts.push_back("state=" + std::string(to_string(s.machine.state())));
  e.facts.push_back("generated=" + std::to_string(s.generated));
  e.facts.push_back("budget=" + std::to_string(s.budget.remaining()));
  if (q == "why_waiting") {
    e.answer = "sequence has " + std::string(to_string(s.machine.state())) + "; lacking a compatible worker/group or memory headroom";
    e.factors.push_back("no_compatible_worker");
    e.factors.push_back("no_memory_headroom");
  } else if (q == "why_ready") {
    e.answer = "sequence is eligible for the next decode iteration";
    e.factors.push_back("ready");
  } else if (q == "why_grouped") {
    e.answer = "sequence shares a compatibility key with its group";
    e.factors.push_back("compatible_key");
  }
  e.facts.push_back("key=" + impl_->key_for_model(s.req).to_string());
  return e;
}

std::uint32_t DecodeFabric::active_sequences() const {
  std::shared_lock lk(impl_->mtx);
  std::uint32_t n = 0;
  for (const auto& kv : impl_->seqs) if (!is_terminal(kv.second.machine.state())) ++n;
  return n;
}

std::uint32_t DecodeFabric::ready_sequences() const {
  std::shared_lock lk(impl_->mtx);
  std::uint32_t n = 0;
  for (const auto& kv : impl_->seqs) if (impl_->is_candidate_state(kv.second.machine.state())) ++n;
  return n;
}

DecodeFabric::DispatchAuthority DecodeFabric::in_flight_authority(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  DispatchAuthority a;
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) return a;
  const Impl::Seq& s = it->second;
  if (s.has_inflight && s.in_flight_dispatch.is_valid()) {
    a.exists = true;
    a.dispatch = s.in_flight_dispatch;
    a.epoch = s.in_flight_epoch;
    a.worker = s.in_flight_worker;
    a.worker_boot = s.in_flight_worker_boot;
    a.attempt = s.current_attempt;
    a.generation = DecodeGeneration::from(s.in_flight_generation);
    a.generated = s.generated;
  }
  return a;
}

std::uint64_t DecodeFabric::stale_rejections() const {
  std::shared_lock lk(impl_->mtx);
  return impl_->stats.stale_rejections;
}

std::uint64_t DecodeFabric::sequence_generated(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) return 0;
  return it->second.generated;
}

SequenceState DecodeFabric::sequence_state(SequenceId seq) const {
  std::shared_lock lk(impl_->mtx);
  auto it = impl_->seqs.find(seq);
  if (it == impl_->seqs.end()) return SequenceState::Admitted;
  return it->second.machine.state();
}

// ===========================================================================
// Persistence (versioned + checksummed)
// ===========================================================================
Result<std::vector<std::uint8_t>> DecodeFabric::serialize_state() const {
  std::shared_lock lk(impl_->mtx);
  binary::Writer w;
  const char magic[] = "DFST";
  w.u8(static_cast<std::uint8_t>(magic[0]));
  w.u8(static_cast<std::uint8_t>(magic[1]));
  w.u8(static_cast<std::uint8_t>(magic[2]));
  w.u8(static_cast<std::uint8_t>(magic[3]));
  w.u32(kPersistenceVersion);
  w.u64(impl_->epoch.value());
  w.u64(impl_->next_seq);
  w.u64(impl_->next_dispatch);
  // sequences
  std::uint64_t count = static_cast<std::uint64_t>(impl_->seqs.size());
  w.u64(count);
  for (const auto& kv : impl_->seqs) {
    const Impl::Seq& s = kv.second;
    w.u64(kv.first.value());
    w.u64(s.req.id.value()); w.u64(s.current_attempt.value());
    w.u64(s.req.tenant.value()); w.u64(s.req.model.value()); w.u64(s.req.revision.value());
    w.u64(s.req.prompt_length); w.u64(s.req.max_generation_length);
    w.u64(s.budget.generated()); w.u64(s.generation);
    w.u64(s.generated); w.u64(s.committed); w.u64(s.current_length);
    w.ns(s.req.deadline.ns); w.u32(s.req.priority);
    w.u8(static_cast<std::uint8_t>(s.req.latency_class));
    w.u8(static_cast<std::uint8_t>(s.machine.state()));
    w.u64(s.state.id.value()); w.u64(s.state.generation);
    w.u64(s.state.bytes_held); w.u64(s.state.estimated_growth);
    w.u64(s.state.owner_tag);
    w.u32(s.attempt_count);
    // Transactional executor-state protocol metadata.
    w.u64(s.committed_state_digest);
    w.u8(s.state_digest_established ? 1 : 0);
  }
  // terminal records
  std::uint64_t tcount = static_cast<std::uint64_t>(impl_->terminal_records.size());
  w.u64(tcount);
  for (const auto& kv : impl_->terminal_records) {
    w.u64(kv.first.value());
    w.u8(static_cast<std::uint8_t>(kv.second));
  }
  // Grant ledger (transactional protocol) + receipt count.
  std::uint64_t gcnt = static_cast<std::uint64_t>(impl_->grant_ledger.size());
  w.u64(gcnt);
  for (const auto& kv : impl_->grant_ledger) {
    const Impl::GrantRecord& gr = kv.second;
    const CommitGrant& g = gr.grant;
    w.u64(g.grant_id.value()); w.u64(g.proposal.value());
    w.u64(g.epoch.value()); w.u64(g.worker.value()); w.u64(g.worker_boot.value());
    w.u64(g.sequence.value()); w.u64(g.state.value()); w.u64(g.attempt.value());
    w.u64(g.generation.value()); w.u64(g.dispatch.value());
    w.u64(g.committed_position);
    w.u64(g.pre_state_digest); w.u64(g.post_state_digest); w.u64(g.delta_digest);
    w.u8(static_cast<std::uint8_t>(g.outcome_kind)); w.u8(g.terminal ? 1 : 0);
    w.u32(g.token_identifier); w.ns(g.active_ns);
    w.u32(static_cast<std::uint32_t>(gr.status));
    w.u8(gr.has_receipt ? 1 : 0);
    if (gr.has_receipt) {
      const MemberReceipt& rp = gr.receipt;
      w.u64(rp.receipt_id.value()); w.u64(rp.grant_id.value()); w.u64(rp.proposal.value());
      w.u64(rp.epoch.value()); w.u64(rp.worker.value()); w.u64(rp.worker_boot.value());
      w.u64(rp.sequence.value()); w.u64(rp.state.value()); w.u64(rp.attempt.value());
      w.u64(rp.generation.value()); w.u64(rp.dispatch.value());
      w.u64(rp.committed_position_before); w.u64(rp.committed_position_after);
      w.u64(rp.pre_state_digest); w.u64(rp.post_state_digest); w.u64(rp.delta_digest);
      w.u8(static_cast<std::uint8_t>(rp.outcome_kind)); w.u8(rp.terminal ? 1 : 0);
      w.u32(rp.token_identifier); w.ns(rp.active_ns); w.ns(rp.committed_at.ns);
    }
  }
  w.u64(impl_->receipt_count);
  std::vector<std::uint8_t> body = w.take();
  std::uint32_t crc = Crc32::update(0xFFFFFFFFu, body.data(), body.size());
  crc = ~crc;
  binary::Writer out;
  out.bytes(body.data(), body.size());
  out.u32(crc);
  return Result<std::vector<std::uint8_t>>::ok(out.take());
}

Result<void> DecodeFabric::recover_state(const std::vector<std::uint8_t>& bytes) {
  std::unique_lock lk(impl_->mtx);
  // Verify the trailing 32-bit CRC over the body before mutating anything.
  if (bytes.size() < 5)
    return failed<void>(ErrorCode::PersistenceTruncated, "state too short");
  std::uint32_t expected_crc = static_cast<std::uint32_t>(bytes[bytes.size() - 4]) |
                              (static_cast<std::uint32_t>(bytes[bytes.size() - 3]) << 8) |
                              (static_cast<std::uint32_t>(bytes[bytes.size() - 2]) << 16) |
                              (static_cast<std::uint32_t>(bytes[bytes.size() - 1]) << 24);
  std::uint32_t computed = Crc32::update(0xFFFFFFFFu, bytes.data(), bytes.size() - 4);
  computed = ~computed;
  if (computed != expected_crc)
    return failed<void>(ErrorCode::PersistenceChecksumMismatch, "state checksum mismatch");
  binary::Reader r(bytes);
  auto m0 = r.u8(); if (!m0.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "missing magic");
  auto m1 = r.u8(); if (!m1.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "missing magic");
  auto m2 = r.u8(); if (!m2.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "missing magic");
  auto m3 = r.u8(); if (!m3.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "missing magic");
  if (m0.value() != 'D' || m1.value() != 'F' || m2.value() != 'S' || m3.value() != 'T')
    return failed<void>(ErrorCode::PersistenceCorrupt, "bad magic");
  auto ver = r.u32(); if (!ver.ok() || ver.value() != kPersistenceVersion)
    return failed<void>(ErrorCode::PersistenceUnknownVersion, "bad persistence version");
  auto ep = r.u64(); if (!ep.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "epoch");
  auto next_seq = r.u64(); if (!next_seq.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "next_seq");
  auto next_d = r.u64(); if (!next_d.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "next_dispatch");
  impl_->epoch = CoordinatorEpoch::from(ep.value());
  impl_->next_seq = next_seq.value();
  impl_->next_dispatch = next_d.value();
  impl_->seqs.clear();
  impl_->req_to_seq.clear();
  impl_->terminal_records.clear();
  impl_->tenant_active.clear();
  auto cnt = r.u64(); if (!cnt.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "seq count");
  for (std::uint64_t i = 0; i < cnt.value(); ++i) {
    Impl::Seq s;
    auto s0 = r.u64(); if (!s0.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "seq id");
    SequenceId seq = SequenceId::from(s0.value());
    auto rq = r.u64(); if (!rq.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "req id");
    auto at = r.u64(); if (!at.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "attempt");
    auto t = r.u64(); if (!t.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "tenant");
    auto md = r.u64(); if (!md.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "model");
    auto rv = r.u64(); if (!rv.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "revision");
    auto pl = r.u64(); if (!pl.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "prompt");
    auto ml = r.u64(); if (!ml.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "maxlen");
    auto bg = r.u64(); if (!bg.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "generated");
    auto gen = r.u64(); if (!gen.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "generation");
    auto gen_t = r.u64(); if (!gen_t.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "g");
    auto comm = r.u64(); if (!comm.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "c");
    auto clen = r.u64(); if (!clen.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "clen");
    auto dl = r.ns(); if (!dl.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "deadline");
    auto pr = r.u32(); if (!pr.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "priority");
    auto lc = r.u8(); if (!lc.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "latency");
    auto st = r.u8(); if (!st.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "state");
    auto sid = r.u64(); if (!sid.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "state id");
    auto sg = r.u64(); if (!sg.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "state gen");
    auto bh = r.u64(); if (!bh.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "bytes held");
    auto eg = r.u64(); if (!eg.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "est growth");
    auto ot = r.u64(); if (!ot.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "owner tag");
    auto ac = r.u32(); if (!ac.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "attempt count");

    s.req.id = RequestId::from(rq.value());
    s.current_attempt = AttemptId::from(at.value());
    s.req.tenant = TenantId::from(t.value());
    s.req.model = ModelId::from(md.value());
    s.req.revision = ModelRevision::from(rv.value());
    s.req.prompt_length = pl.value();
    s.req.max_generation_length = ml.value();
    s.req.deadline = TimePoint(dl.value());
    s.req.priority = pr.value();
    s.req.latency_class = static_cast<LatencyClass>(lc.value());
    s.budget = TokenBudget(ml.value());
    for (std::uint64_t k = 0; k < bg.value(); ++k) s.budget.advance(1);
    s.generation = gen.value();
    s.generated = gen_t.value();
    s.committed = comm.value();
    s.current_length = clen.value();
    s.state.id = StateId::from(sid.value());
    s.state.generation = sg.value();
    s.state.bytes_held = bh.value();
    s.state.estimated_growth = eg.value();
    s.state.owner_tag = ot.value();
    s.attempt_count = ac.value();
    // Transactional executor-state protocol metadata.
    auto csd = r.u64(); if (!csd.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "committed digest");
    auto sde = r.u8(); if (!sde.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "digest established");
    s.committed_state_digest = csd.value();
    s.state_digest_established = sde.value() != 0;
    // Never restore a stale in-flight authority as current.
    s.has_inflight = false;
    s.in_flight_dispatch = DispatchId::null();
    s.has_inflight_grant = false;
    s.in_flight_grant = GrantId::null();
    s.in_flight_proposal = ProposalId::null();
    SequenceState restored = static_cast<SequenceState>(st.value());
    if (restored == SequenceState::Dispatched || restored == SequenceState::Running ||
        restored == SequenceState::Reserved || restored == SequenceState::Grouped ||
        restored == SequenceState::StepCompleted || restored == SequenceState::Cancelled ||
        restored == SequenceState::DeadlineExpired) {
      // Reconcile formerly-running work explicitly: re-queue, clear reservations.
      s.reservation = ReservationId::null();
      s.machine = SequenceStateMachine(restored);
      s.machine.transition_to(SequenceState::Waiting);
    } else {
      s.machine = SequenceStateMachine(restored);
    }
    impl_->seqs.emplace(seq, std::move(s));
    impl_->req_to_seq[s.req.id] = seq;
    impl_->tenant_active[s.req.tenant]++;
  }
  auto tcnt = r.u64(); if (!tcnt.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "terminal count");
  for (std::uint64_t i = 0; i < tcnt.value(); ++i) {
    auto tseq = r.u64(); if (!tseq.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "t seq");
    auto tst = r.u8(); if (!tst.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "t state");
    impl_->terminal_records[SequenceId::from(tseq.value())] = static_cast<SequenceState>(tst.value());
  }
  // Grant ledger (transactional protocol) + receipt count.
  impl_->grant_ledger.clear();
  impl_->receipt_count = 0;
  auto gcnt = r.u64(); if (!gcnt.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant count");
  if (gcnt.value() > (1ull << 22)) return failed<void>(ErrorCode::PersistenceInvalidField, "grant count too large");
  for (std::uint64_t i = 0; i < gcnt.value(); ++i) {
    Impl::GrantRecord gr;
    auto gid = r.u64(); if (!gid.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant id");
    auto pro = r.u64(); if (!pro.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant proposal");
    auto g_ep = r.u64(); if (!g_ep.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant epoch");
    auto wk = r.u64(); if (!wk.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant worker");
    auto wb = r.u64(); if (!wb.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant boot");
    auto sq = r.u64(); if (!sq.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant seq");
    auto st2 = r.u64(); if (!st2.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant state");
    auto at = r.u64(); if (!at.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant attempt");
    auto gn = r.u64(); if (!gn.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant generation");
    auto di = r.u64(); if (!di.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant dispatch");
    auto cp = r.u64(); if (!cp.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant position");
    auto pre = r.u64(); if (!pre.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant pre");
    auto post = r.u64(); if (!post.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant post");
    auto dd = r.u64(); if (!dd.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant delta");
    auto ok = r.u8(); if (!ok.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant ok");
    auto term = r.u8(); if (!term.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant term");
    auto tok = r.u32(); if (!tok.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant token");
    auto an = r.ns(); if (!an.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant active");
    auto status = r.u32(); if (!status.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant status");
    auto has_rec = r.u8(); if (!has_rec.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "grant has receipt");
    gr.grant.grant_id = GrantId::from(gid.value()); gr.grant.proposal = ProposalId::from(pro.value());
    gr.grant.epoch = CoordinatorEpoch::from(g_ep.value()); gr.grant.worker = WorkerId::from(wk.value());
    gr.grant.worker_boot = WorkerBootId::from(wb.value()); gr.grant.sequence = SequenceId::from(sq.value());
    gr.grant.state = StateId::from(st2.value()); gr.grant.attempt = AttemptId::from(at.value());
    gr.grant.generation = DecodeGeneration::from(gn.value()); gr.grant.dispatch = DispatchId::from(di.value());
    gr.grant.committed_position = cp.value(); gr.grant.pre_state_digest = pre.value();
    gr.grant.post_state_digest = post.value(); gr.grant.delta_digest = dd.value();
    gr.grant.outcome_kind = static_cast<MemberOutcomeKind>(ok.value()); gr.grant.terminal = term.value() != 0;
    gr.grant.token_identifier = tok.value(); gr.grant.active_ns = an.value();
    // A pending grant must not be revived after restart: it becomes stale.
    gr.status = (status.value() == 0) ? 2 : static_cast<int>(status.value());
    if (has_rec.value() != 0) {
      MemberReceipt rp;
      auto ra = r.u64(); if (!ra.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt id");
      auto rb = r.u64(); if (!rb.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt grant");
      auto rc = r.u64(); if (!rc.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt proposal");
      auto rd = r.u64(); if (!rd.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt epoch");
      auto re = r.u64(); if (!re.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt worker");
      auto rf = r.u64(); if (!rf.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt boot");
      auto rg = r.u64(); if (!rg.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt seq");
      auto rh = r.u64(); if (!rh.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt state");
      auto ri = r.u64(); if (!ri.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt attempt");
      auto rj = r.u64(); if (!rj.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt gen");
      auto rk = r.u64(); if (!rk.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt dispatch");
      auto la = r.u64(); if (!la.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt cb");
      auto lb = r.u64(); if (!lb.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt ca");
      auto lc = r.u64(); if (!lc.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt pre");
      auto ld = r.u64(); if (!ld.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt post");
      auto le = r.u64(); if (!le.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt delta");
      auto lf = r.u8(); if (!lf.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt kind");
      auto lg = r.u8(); if (!lg.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt terminal");
      auto lh = r.u32(); if (!lh.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt token");
      auto li = r.ns(); if (!li.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt active");
      auto lj = r.ns(); if (!lj.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt time");
      rp.receipt_id = ReceiptId::from(ra.value()); rp.grant_id = GrantId::from(rb.value());
      rp.proposal = ProposalId::from(rc.value()); rp.epoch = CoordinatorEpoch::from(rd.value());
      rp.worker = WorkerId::from(re.value()); rp.worker_boot = WorkerBootId::from(rf.value());
      rp.sequence = SequenceId::from(rg.value()); rp.state = StateId::from(rh.value());
      rp.attempt = AttemptId::from(ri.value()); rp.generation = DecodeGeneration::from(rj.value());
      rp.dispatch = DispatchId::from(rk.value());
      rp.committed_position_before = la.value(); rp.committed_position_after = lb.value();
      rp.pre_state_digest = lc.value(); rp.post_state_digest = ld.value(); rp.delta_digest = le.value();
      rp.outcome_kind = static_cast<MemberOutcomeKind>(lf.value()); rp.terminal = lg.value() != 0;
      rp.token_identifier = lh.value(); rp.active_ns = li.value(); rp.committed_at = TimePoint(lj.value());
      gr.receipt = std::move(rp);
      gr.has_receipt = true;
    }
    impl_->grant_ledger[gr.grant.grant_id] = std::move(gr);
  }
  auto rcnt = r.u64(); if (!rcnt.ok()) return failed<void>(ErrorCode::PersistenceTruncated, "receipt count");
  impl_->receipt_count = rcnt.value();
  // Recompute derived/total counters from the restored sequences (the raw
  // aggregate counters are derived, never trusted from the wire).
  impl_->stats.generated_tokens = 0;
  impl_->stats.decode_steps = 0;
  impl_->stats.sequences_started = static_cast<std::uint64_t>(impl_->seqs.size());
  impl_->stats.requests_admitted = static_cast<std::uint64_t>(impl_->req_to_seq.size());
  for (const auto& kv : impl_->seqs) {
    impl_->stats.generated_tokens += kv.second.generated;
    if (kv.second.generation >= 1) impl_->stats.decode_steps += (kv.second.generation - 1);
  }
  impl_->record_event(EventKind::MemoryReconciled, {}, {}, "recovered");
  return Result<void>::success();
}

}  // namespace decodefabric
