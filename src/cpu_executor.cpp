#include "decodefabric/cpu_executor.hpp"
#include <chrono>
#include <cmath>
#include <cstring>

namespace decodefabric {

namespace {
inline std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}
inline std::uint64_t digest_state(const std::vector<double>& s,
                                  std::uint64_t tokens, std::uint64_t kv_bytes) {
  std::uint64_t h = CpuDecodeExecutor::state_checksum(s);
  h = fnv_mix(h, tokens);
  h = fnv_mix(h, kv_bytes);
  return h;
}

// Deterministic single decode step: the tanh recurrence then bounded
// renormalization. Used both by prepare() and by the deterministic state
// reconstruction contract (a fresh worker rebuilds the committed state by
// applying exactly the authoritative committed-token count of these steps).
inline std::vector<double> step_state(const std::vector<double>& state) {
  const std::size_t N = state.size();
  std::vector<double> next(N);
  for (std::size_t i = 0; i < N; ++i) {
    double prev = state[(i + 1) % N];
    double acc = 0.5 * state[i] + 0.25 * prev;
    next[i] = std::tanh(acc + 0.1 * std::sin(static_cast<double>(i)));
  }
  double norm = 0.0;
  for (double v : next) norm += v * v;
  if (norm > 0.0) { double inv = 1.0 / std::sqrt(norm); for (double& v : next) v *= inv; }
  return next;
}
}  // namespace

std::uint64_t CpuDecodeExecutor::kStateSize = 8;

CpuDecodeExecutor::CpuDecodeExecutor(DeviceId device_id)
    : device_id_(device_id), id_(ExecutorId::from(1)) {
  device_.id = device_id;
  device_.backend = BackendKind::CPU;
  device_.name = "DecodeFabric CPU decode device";
  device_.compute_capability_major = 0;
  device_.compute_capability_minor = 0;
  device_.memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;  // advertized host budget
  device_.supported_dtypes = (1u << static_cast<std::uint32_t>(DType::F32)) |
                             (1u << static_cast<std::uint32_t>(DType::F64));
}

ExecutorId CpuDecodeExecutor::id() const { return id_; }
BackendKind CpuDecodeExecutor::backend() const { return BackendKind::CPU; }
DeviceDescriptor CpuDecodeExecutor::device() const { return device_; }

bool CpuDecodeExecutor::supports(const CompatibilityKey& key) const {
  return key.backend == BackendKind::CPU && key.device == device_id_;
}

std::vector<double> CpuDecodeExecutor::initial_state(StateId sid, std::uint64_t seed) {
  const std::uint64_t N = kStateSize;
  std::vector<double> s(N);
  std::uint64_t h = seed ^ (sid.value() * 0x9E3779B97F4A7C15ull);
  for (std::uint64_t i = 0; i < N; ++i) {
    h ^= h >> 12; h ^= h << 25; h ^= h >> 27;
    std::uint64_t v = h * 0x2545F4914F6CDD1Dull;
    double d = static_cast<double>(v >> 11) / static_cast<double>(1ull << 53);  // [0,1)
    s[i] = (d * 2.0 - 1.0) * 0.5;  // bounded in [-0.5, 0.5]
  }
  return s;
}

std::uint64_t CpuDecodeExecutor::state_checksum(const std::vector<double>& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (double d : s) {
    // Pack the double's exact bits so the checksum is bit-deterministic.
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(d), "sizeof mismatch");
    std::memcpy(&bits, &d, sizeof(bits));
    for (int i = 0; i < 8; ++i) { h ^= (bits >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
  }
  return h;
}

std::uint32_t CpuDecodeExecutor::step_token(const std::vector<double>& s) {
  // Deterministic argmax over a numerically-transformed state vector.
  std::uint32_t best = 0;
  double bestv = -1e300;
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(s.size()); ++i) {
    double v = s[i] * static_cast<double>(i + 1) + std::sin(s[i]);
    if (v > bestv) { bestv = v; best = i; }
  }
  return best;
}

Result<void> CpuDecodeExecutor::ensure_initialized(SeqState& st, const DecodeMemberSpec& m) {
  // Deterministic S0 from (state.id, attempt); the seed never depends on the
  // generated-token count so that a fresh worker reproduces the same S0.
  if (st.state.empty()) {
    st.state = initial_state(m.state.id, m.attempt.value());
    st.tokens = 0;
    st.kv_bytes = m.state.bytes_held ? m.state.bytes_held : 0;
    st.eos_enabled = false;
    st.eos_target = 0;
    if (m.payload.size() >= 1) { st.eos_enabled = (m.payload[0] & 0x01) != 0; }
    if (m.payload.size() >= 2) { st.eos_target = m.payload[1]; }
  }
  // Deterministic reconstruction/catch-up: if this worker's local committed
  // state is behind the authoritative committed-token count (either it is
  // adopting a sequence whose committed state lives on another worker, or a
  // restarted worker), advance it forward to match. This makes the
  // committed-state digest and committed-token position consistent with the
  // coordinator, so any worker can serve any sequence and a restarted worker
  // re-derives the exact same state.
  const std::uint64_t delta = 64;
  while (st.tokens < m.generated_tokens) {
    st.state = step_state(st.state);
    st.tokens += 1;
    st.kv_bytes += delta;
  }
  return Result<void>::success();
}

void CpuDecodeExecutor::clear_tentative(SeqState& st) {
  st.has_tentative = false;
  st.tentative_state.clear();
  st.tentative_kv_bytes = 0;
  st.prepared_proposal = ProposalId::null();
  st.prepared_attempt = AttemptId::null();
  st.prepared_generation = DecodeGeneration::null();
  st.prepared_sequence = SequenceId::null();
  st.prepared_worker = WorkerId::null();
  st.prepared_worker_boot = WorkerBootId::null();
  st.prepared_epoch = CoordinatorEpoch::null();
  st.prepared_dispatch = DispatchId::null();
  st.prepared_pre_digest = 0;
  st.prepared_post_digest = 0;
}

Result<PreparedMember> CpuDecodeExecutor::prepare_member(
    const DecodeMemberSpec& m, const DecodeExecutionRequest& req, Nanoseconds* active_ns) {
  auto t0 = std::chrono::steady_clock::now();
  // mu_ is held by the caller.
  SeqState& st = states_[m.state.id];
  {
    auto r = ensure_initialized(st, m);
    if (!r.ok()) return failed<PreparedMember>(r.error().code, r.error().message);
  }
  // Do not allow a second prepare for the same authoritative sequence
  // generation to silently overwrite an unresolved prepared transition.
  if (st.has_tentative) {
    if (st.prepared_attempt == m.attempt && st.prepared_generation == m.generation) {
      return failed<PreparedMember>(ErrorCode::DuplicateCompletion,
                                    "duplicate prepare for the same authoritative generation");
    }
    if (st.prepared_attempt != m.attempt) {
      // A retry mints a NEW AttemptId; the old tentative belongs to a stale
      // attempt, so discard it before preparing the fresh authoritative step.
      clear_tentative(st);
    } else {
      // Same attempt but a HIGHER generation arrived while the previous
      // generation's transition is still unresolved (not committed). Committing
      // it later would be impossible after we overwrite the tentative, so reject
      // the out-of-order prepare and let the previous generation commit first.
      return failed<PreparedMember>(ErrorCode::TransactionConflict,
                                    "prepare out of order: prior generation unresolved");
    }
  }

  // --- Prepare from the committed pre-state only. ---
  std::vector<double> next = step_state(st.state);
  std::uint32_t token = step_token(next);

  std::uint64_t delta = 64;  // per-token KV growth (bytes)
  std::uint64_t before_tokens = st.tokens;
  std::uint64_t before_kv = st.kv_bytes;
  std::uint64_t pre = digest_state(st.state, before_tokens, before_kv);
  std::uint64_t post = digest_state(next, before_tokens + 1, before_kv + delta);
  std::uint64_t delta_digest = fnv_mix(pre, post);

  // Store the tentative transition (committed state unchanged).
  st.has_tentative = true;
  st.tentative_state = std::move(next);
  st.tentative_kv_bytes = before_kv + delta;
  st.prepared_proposal = ProposalId::from(++next_proposal_);
  st.prepared_attempt = m.attempt;
  st.prepared_generation = m.generation;
  st.prepared_sequence = m.sequence;
  st.prepared_worker = req.worker;
  st.prepared_worker_boot = req.worker_boot;
  st.prepared_epoch = req.epoch;
  st.prepared_dispatch = req.dispatch_id;
  st.prepared_pre_digest = pre;
  st.prepared_post_digest = post;

  auto t1 = std::chrono::steady_clock::now();
  Nanoseconds ns = static_cast<Nanoseconds>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  if (active_ns) *active_ns += ns;

  PreparedMember pm;
  pm.sequence = m.sequence;
  pm.state = m.state.id;
  pm.attempt = m.attempt;
  pm.generation = m.generation;
  pm.dispatch = DispatchId::null();  // filled by prepare() from the request
  pm.epoch = CoordinatorEpoch::null();
  pm.worker = WorkerId::null();
  pm.worker_boot = WorkerBootId::null();
  pm.proposal = st.prepared_proposal;
  pm.pre_state_digest = pre;
  pm.post_state_digest = post;
  pm.delta_digest = delta_digest;
  pm.committed_position_before = before_tokens;
  pm.committed_position_after = before_tokens + 1;
  pm.active_ns = ns;

  MemberOutcome mo;
  mo.sequence = m.sequence;
  mo.kind = MemberOutcomeKind::StepSuccessContinue;
  mo.attempt = m.attempt;
  mo.generation = m.generation;
  mo.generated = 1;
  mo.token_identifier = token;
  mo.terminal = false;
  mo.started_at = TimePoint(0);
  mo.finished_at = TimePoint(ns);
  mo.active_ns = ns;
  mo.kv_bytes_delta = delta;
  mo.kv_bytes_after = before_kv + delta;
  mo.retryable = false;

  std::uint64_t max = m.generated_tokens + m.remaining_budget;
  if ((st.eos_enabled && token == st.eos_target) || (m.generated_tokens + 1 >= max)) {
    mo.kind = MemberOutcomeKind::StepSuccessTerminal;
    mo.terminal = true;
  }
  pm.outcome = std::move(mo);
  return Result<PreparedMember>::ok(std::move(pm));
}

Result<PreparedDecode> CpuDecodeExecutor::prepare(const DecodeExecutionRequest& req) {
  std::lock_guard<std::mutex> lk(mu_);
  PreparedDecode out;
  out.dispatch_id = req.dispatch_id;
  out.epoch = req.epoch;
  out.worker = req.worker;
  out.worker_boot = req.worker_boot;
  Nanoseconds total = 0;
  for (const DecodeMemberSpec& m : req.members) {
    auto r = prepare_member(m, req, &total);
    if (!r.ok()) {
      // A prepare failure for one member fails the group prepare; no committed
      // state was mutated. Any tentative already staged for other members is
      // left unresolved and will be aborted by the caller.
      out.group_error = r.error().code;
      out.group_error_message = r.error().message;
      return Result<PreparedDecode>{r.error()};
    }
    PreparedMember pm = std::move(r.value());
    pm.dispatch = req.dispatch_id;
    pm.epoch = req.epoch;
    pm.worker = req.worker;
    pm.worker_boot = req.worker_boot;
    out.members.push_back(std::move(pm));
  }
  out.group_active_ns = total;
  out.group_error = ErrorCode::Ok;
  return Result<PreparedDecode>::ok(std::move(out));
}

Result<MemberReceipt> CpuDecodeExecutor::commit(const CommitGrant& grant) {
  std::lock_guard<std::mutex> lk(mu_);

  // Idempotent duplicate commit: if this grant was already consumed and the
  // existing receipt matches the grant identity exactly, return it unchanged.
  auto rit = receipts_by_grant_.find(grant.grant_id);
  if (rit != receipts_by_grant_.end()) {
    const MemberReceipt& existing = rit->second;
    bool same = existing.grant_id == grant.grant_id &&
                existing.proposal == grant.proposal &&
                existing.sequence == grant.sequence &&
                existing.state == grant.state &&
                existing.attempt == grant.attempt &&
                existing.generation == grant.generation &&
                existing.worker_boot == grant.worker_boot &&
                existing.epoch == grant.epoch &&
                existing.dispatch == grant.dispatch &&
                existing.committed_position_before == grant.committed_position &&
                existing.pre_state_digest == grant.pre_state_digest &&
                existing.post_state_digest == grant.post_state_digest;
    if (same) return Result<MemberReceipt>::ok(existing);
    return failed<MemberReceipt>(ErrorCode::DuplicateCompletion,
                                 "conflicting reuse of an already-consumed commit grant");
  }

  // Locate the exact prepared transition.
  auto sit = states_.find(grant.state);
  if (sit == states_.end() || !sit->second.has_tentative) {
    return failed<MemberReceipt>(ErrorCode::UnknownState, "no prepared transition for grant state");
  }
  SeqState& st = sit->second;
  if (st.prepared_proposal != grant.proposal ||
      st.prepared_attempt != grant.attempt ||
      st.prepared_generation != grant.generation ||
      st.prepared_sequence != grant.sequence ||
      st.prepared_worker != grant.worker ||
      st.prepared_worker_boot != grant.worker_boot ||
      st.prepared_epoch != grant.epoch ||
      st.prepared_dispatch != grant.dispatch) {
    return failed<MemberReceipt>(ErrorCode::SupersededByRetry, "grant proposal/authority mismatch");
  }
  if (st.prepared_pre_digest != grant.pre_state_digest ||
      st.prepared_post_digest != grant.post_state_digest) {
    return failed<MemberReceipt>(ErrorCode::StateDigestMismatch, "grant digest mismatch");
  }
  if (st.tokens != grant.committed_position) {
    return failed<MemberReceipt>(ErrorCode::InvalidArgument, "grant committed-position mismatch");
  }

  // Atomically promote tentative -> committed.
  std::uint64_t before = st.tokens;
  std::uint64_t after = before + 1;
  std::uint64_t pre = st.prepared_pre_digest;
  std::uint64_t post = st.prepared_post_digest;
  st.state = std::move(st.tentative_state);
  st.tokens = after;
  st.kv_bytes = st.tentative_kv_bytes;
  clear_tentative(st);
  consumed_proposals_[grant.proposal] = true;

  MemberReceipt rec;
  rec.receipt_id = ReceiptId::from(grant.grant_id.value());  // deterministic
  rec.grant_id = grant.grant_id;
  rec.proposal = grant.proposal;
  rec.epoch = grant.epoch;
  rec.worker = grant.worker;
  rec.worker_boot = grant.worker_boot;
  rec.sequence = grant.sequence;
  rec.state = grant.state;
  rec.attempt = grant.attempt;
  rec.generation = grant.generation;
  rec.dispatch = grant.dispatch;
  rec.committed_position_before = before;
  rec.committed_position_after = after;
  rec.pre_state_digest = pre;
  rec.post_state_digest = post;
  rec.delta_digest = grant.delta_digest;
  rec.outcome_kind = grant.outcome_kind;
  rec.terminal = grant.terminal;
  rec.token_identifier = grant.token_identifier;
  rec.active_ns = grant.active_ns;
  rec.committed_at = TimePoint(0);  // metadata filled by coordinator
  receipts_by_grant_[grant.grant_id] = rec;
  return Result<MemberReceipt>::ok(std::move(rec));
}

Result<void> CpuDecodeExecutor::abort(const AbortPrepared& abort) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = consumed_proposals_.find(abort.proposal);
  if (it != consumed_proposals_.end()) {
    // Abort after a successful commit: committed state must remain unchanged.
    return failed<void>(ErrorCode::AlreadyTerminal, "abort after commit");
  }
  auto sit = states_.find(abort.state);
  if (sit == states_.end()) return Result<void>::success();
  SeqState& st = sit->second;
  if (st.has_tentative && st.prepared_proposal == abort.proposal) {
    clear_tentative(st);
  }
  // Aborting an already-aborted or unknown proposal is an idempotent no-op.
  return Result<void>::success();
}

std::uint64_t CpuDecodeExecutor::committed_state_digest(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return 0;
  return digest_state(it->second.state, it->second.tokens, it->second.kv_bytes);
}

bool CpuDecodeExecutor::has_prepared(StateId id, ProposalId proposal) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  return it->second.has_tentative && it->second.prepared_proposal == proposal;
}

std::uint64_t CpuDecodeExecutor::tentative_tokens(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return 0;
  // If a transition is prepared (not committed), the tentative post-step token
  // count is committed+1; otherwise it is the committed count.
  return it->second.tokens + (it->second.has_tentative ? 1 : 0);
}

std::uint64_t CpuDecodeExecutor::committed_tokens(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  return it == states_.end() ? 0 : it->second.tokens;
}

}  // namespace decodefabric
