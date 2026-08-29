#include "decodefabric/cuda_executor.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace decodefabric {

namespace {
inline std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}
inline std::uint64_t digest_dev(const std::vector<float>& s,
                                std::uint64_t tokens, std::uint64_t kv_bytes) {
  std::uint64_t h = CudaDecodeExecutor::state_checksum(s);
  h = fnv_mix(h, tokens);
  h = fnv_mix(h, kv_bytes);
  return h;
}

// Host mirror of the device decode step (no renormalization, matching the
// kernel exactly). Used by the deterministic reconstruction contract.
inline std::vector<float> step_state_float(std::vector<float> in) {
  const std::size_t N = in.size();
  std::vector<float> out(N);
  for (std::size_t i = 0; i < N; ++i) {
    float prev = in[(i + 1) % N];
    out[i] = tanhf(0.5f * in[i] + 0.25f * prev + 0.1f * sinf(static_cast<float>(i)));
  }
  return out;
}
}  // namespace

__global__ void df_decode_step_kernel(const float* __restrict__ in, float* __restrict__ out,
                                      std::uint32_t* __restrict__ token,
                                      std::uint64_t* __restrict__ cksum, int n) {
  int i = threadIdx.x;
  if (i < n) {
    float prev = in[(i + 1) % n];
    float acc = 0.5f * in[i] + 0.25f * prev + 0.1f * sinf(static_cast<float>(i));
    out[i] = tanhf(acc);
  }
  __syncthreads();
  if (i == 0) {
    std::uint32_t best = 0;
    float bestv = -1e30f;
    std::uint64_t h = 1469598103934665603ull;
    for (int j = 0; j < n; ++j) {
      float v = out[j] * static_cast<float>(j + 1) + sinf(out[j]);
      if (v > bestv) { bestv = v; best = static_cast<std::uint32_t>(j); }
      std::uint64_t bits = 0;
      {
        const unsigned char* bp = reinterpret_cast<const unsigned char*>(&out[j]);
        for (int k = 0; k < 4; ++k) bits |= (static_cast<std::uint64_t>(bp[k]) << (8 * k));
      }
      for (int k = 0; k < 4; ++k) { h ^= (bits >> (8 * k)) & 0xFF; h *= 1099511628211ull; }
    }
    *token = best;
    *cksum = h;
  }
}

CudaDecodeExecutor::CudaDecodeExecutor(DeviceId device_id, int cuda_device)
    : device_id_(device_id), id_(ExecutorId::from(2)), cuda_device_(cuda_device) {
  device_.id = device_id;
  device_.backend = BackendKind::CUDA;
  device_.compute_capability_major = 0;
  device_.compute_capability_minor = 0;
  cudaDeviceProp prop{};
  if (cudaGetDeviceProperties(&prop, cuda_device) == cudaSuccess) {
    device_.name = prop.name;
    device_.compute_capability_major = prop.major;
    device_.compute_capability_minor = prop.minor;
    device_.memory_bytes = prop.totalGlobalMem;
    device_.supported_dtypes = (1u << static_cast<std::uint32_t>(DType::F32));
    device_.max_groups_concurrent = 1;
  } else {
    device_.name = "cuda_device_unavailable";
    device_.supported_dtypes = 0;
  }
}

CudaDecodeExecutor::~CudaDecodeExecutor() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& kv : states_) {
    if (kv.second.device) cudaFree(kv.second.device);
    if (kv.second.tentative) cudaFree(kv.second.tentative);
    kv.second.device = nullptr;
    kv.second.tentative = nullptr;
  }
  states_.clear();
}

ExecutorId CudaDecodeExecutor::id() const { return id_; }
BackendKind CudaDecodeExecutor::backend() const { return BackendKind::CUDA; }
DeviceDescriptor CudaDecodeExecutor::device() const { return device_; }
bool CudaDecodeExecutor::supports(const CompatibilityKey& key) const {
  return key.backend == BackendKind::CUDA && key.device == device_id_;
}

std::vector<float> CudaDecodeExecutor::initial_state(StateId sid, std::uint64_t seed) {
  const std::uint64_t N = kStateSize;
  std::vector<float> s(N);
  std::uint64_t h = seed ^ (sid.value() * 0x9E3779B97F4A7C15ull);
  for (std::uint64_t i = 0; i < N; ++i) {
    h ^= h >> 12; h ^= h << 25; h ^= h >> 27;
    std::uint64_t v = h * 0x2545F4914F6CDD1Dull;
    double d = static_cast<double>(v >> 11) / static_cast<double>(1ull << 53);
    s[i] = static_cast<float>((d * 2.0 - 1.0) * 0.5);
  }
  return s;
}

std::uint64_t CudaDecodeExecutor::state_checksum(const std::vector<float>& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (float din : s) {
    // Hash exactly the 4 bytes of the float so the digest is deterministic and
    // stable (the float is 4 bytes, not 8; reading 8 would consume garbage).
    std::uint32_t bits; std::memcpy(&bits, &din, sizeof(bits));
    for (int i = 0; i < 4; ++i) { h ^= (bits >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
  }
  return h;
}

std::uint32_t CudaDecodeExecutor::step_token(const std::vector<float>& s) {
  std::uint32_t best = 0; float bestv = -1e30f;
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(s.size()); ++i) {
    float v = s[i] * static_cast<float>(i + 1) + std::sin(s[i]);
    if (v > bestv) { bestv = v; best = i; }
  }
  return best;
}

Result<void> CudaDecodeExecutor::ensure_initialized(DevState& st, const DecodeMemberSpec& m) {
  if (!st.inited) {
    cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&st.device), kStateSize * sizeof(float));
    if (e != cudaSuccess) return failed<void>(ErrorCode::BackendError, "cudaMalloc committed failed");
    std::vector<float> init = initial_state(m.state.id, m.attempt.value());
    e = cudaMemcpy(st.device, init.data(), kStateSize * sizeof(float), cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { cudaFree(st.device); st.device = nullptr; return failed<void>(ErrorCode::BackendError, "cudaMemcpy init failed"); }
    st.kv_bytes = m.state.bytes_held ? m.state.bytes_held : 0;
    st.tokens = 0;
    st.token = 0;
    st.checksum = 0;
    st.inited = true;
  }
  // Deterministic reconstruction/catch-up: if this worker's local committed
  // device state is behind the authoritative committed-token count (sequence
  // adopted from another worker, or a restarted worker), advance it forward so
  // the committed-state digest and committed-token position match the
  // coordinator. Bounded host copyback for the reconstruction steps.
  if (st.tokens < m.generated_tokens) {
    std::vector<float> host(kStateSize);
    if (cudaMemcpy(host.data(), st.device, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
      return failed<void>(ErrorCode::BackendError, "cudaMemcpy reconstruction read failed");
    const std::uint64_t delta = 64;
    while (st.tokens < m.generated_tokens) {
      host = step_state_float(std::move(host));
      st.tokens += 1;
      st.kv_bytes += delta;
    }
    if (cudaMemcpy(st.device, host.data(), kStateSize * sizeof(float), cudaMemcpyHostToDevice) != cudaSuccess)
      return failed<void>(ErrorCode::BackendError, "cudaMemcpy reconstruction write failed");
  }
  return Result<void>::success();
}

void CudaDecodeExecutor::clear_tentative(DevState& st) {
  if (st.tentative) { cudaFree(st.tentative); st.tentative = nullptr; }
  st.has_tentative = false;
  st.prepared_proposal = ProposalId::null();
  st.prepared_attempt = AttemptId::null();
  st.prepared_generation = DecodeGeneration::null();
  st.prepared_sequence = SequenceId::null();
  st.prepared_worker = WorkerId::null();
  st.prepared_worker_boot = WorkerBootId::null();
  st.prepared_epoch = CoordinatorEpoch::null();
  st.prepared_dispatch = DispatchId::null();
  st.tentative_kv_bytes = 0;
  st.tentative_token = 0;
  st.tentative_checksum = 0;
}

Result<PreparedMember> CudaDecodeExecutor::prepare_member(const DecodeMemberSpec& m,
                                                          const DecodeExecutionRequest& req,
                                                          Nanoseconds* active_ns) {
  auto t0 = std::chrono::steady_clock::now();
  // mu_ is held by the caller.
  cudaError_t err = cudaSetDevice(cuda_device_);
  if (err != cudaSuccess)
    return failed<PreparedMember>(ErrorCode::BackendError,
                                  "cudaSetDevice failed: " + std::string(cudaGetErrorString(err)));

  DevState& st = states_[m.state.id];
  {
    auto r = ensure_initialized(st, m);
    if (!r.ok()) return failed<PreparedMember>(r.error().code, r.error().message);
  }
  if (st.has_tentative) {
    if (st.prepared_attempt == m.attempt && st.prepared_generation == m.generation) {
      return failed<PreparedMember>(ErrorCode::DuplicateCompletion,
                                    "duplicate prepare for the same authoritative generation");
    }
    if (st.prepared_attempt != m.attempt) {
      // A retry mints a NEW AttemptId; discard the stale tentative and prepare
      // the fresh authoritative step.
      clear_tentative(st);
    } else {
      // Same attempt, higher generation arrived while the previous generation
      // is still unresolved. Reject rather than overwrite so the prior
      // generation can commit first.
      return failed<PreparedMember>(ErrorCode::TransactionConflict,
                                    "prepare out of order: prior generation unresolved");
    }
  }

  // Commit state digest (copyback for hashing).
  std::vector<float> host_committed(kStateSize);
  if (cudaMemcpy(host_committed.data(), st.device, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
    return failed<PreparedMember>(ErrorCode::BackendError, "cudaMemcpy committed failed");
  std::uint64_t pre = digest_dev(host_committed, st.tokens, st.kv_bytes);

  // Allocate the tentative device buffer.
  if (!st.tentative) {
    if (cudaMalloc(reinterpret_cast<void**>(&st.tentative), kStateSize * sizeof(float)) != cudaSuccess)
      return failed<PreparedMember>(ErrorCode::BackendError, "cudaMalloc tentative failed");
  }
  std::uint32_t* d_token = nullptr; std::uint64_t* d_cksum = nullptr;
  if (cudaMalloc(reinterpret_cast<void**>(&d_token), sizeof(std::uint32_t)) != cudaSuccess) { clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaMalloc token failed"); }
  if (cudaMalloc(reinterpret_cast<void**>(&d_cksum), sizeof(std::uint64_t)) != cudaSuccess) { cudaFree(d_token); clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaMalloc cksum failed"); }

  df_decode_step_kernel<<<1, kStateSize>>>(st.device, st.tentative, d_token, d_cksum, static_cast<int>(kStateSize));
  cudaError_t ke = cudaGetLastError();
  if (ke != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "kernel launch failed: " + std::string(cudaGetErrorString(ke))); }
  cudaError_t se = cudaDeviceSynchronize();
  if (se != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaDeviceSynchronize failed: " + std::string(cudaGetErrorString(se))); }

  std::uint32_t host_token = 0;
  std::uint64_t host_cksum = 0;
  if (cudaMemcpy(&host_token, d_token, sizeof(std::uint32_t), cudaMemcpyDeviceToHost) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaMemcpy token failed"); }
  if (cudaMemcpy(&host_cksum, d_cksum, sizeof(std::uint64_t), cudaMemcpyDeviceToHost) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaMemcpy cksum failed"); }
  cudaFree(d_token); cudaFree(d_cksum);

  // Post-state digest from the tentative buffer (bounded copyback for hashing).
  std::vector<float> host_tentative(kStateSize);
  if (cudaMemcpy(host_tentative.data(), st.tentative, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess) { clear_tentative(st); return failed<PreparedMember>(ErrorCode::BackendError, "cudaMemcpy tentative failed"); }
  std::uint64_t delta = 64;
  std::uint64_t before_tokens = st.tokens;
  std::uint64_t post = digest_dev(host_tentative, before_tokens + 1, st.kv_bytes + delta);
  std::uint64_t delta_digest = fnv_mix(pre, post);

  st.has_tentative = true;
  st.prepared_proposal = ProposalId::from(++next_proposal_);
  st.prepared_attempt = m.attempt;
  st.prepared_generation = m.generation;
  st.prepared_sequence = m.sequence;
  st.prepared_worker = req.worker;
  st.prepared_worker_boot = req.worker_boot;
  st.prepared_epoch = req.epoch;
  st.prepared_dispatch = req.dispatch_id;
  st.tentative_kv_bytes = st.kv_bytes + delta;
  st.tentative_token = host_token;
  st.tentative_checksum = host_cksum;
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
  pm.dispatch = DispatchId::null();
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
  mo.token_identifier = host_token;
  mo.terminal = false;
  mo.started_at = TimePoint(0);
  mo.finished_at = TimePoint(ns);
  mo.active_ns = ns;
  mo.kv_bytes_delta = delta;
  mo.kv_bytes_after = st.kv_bytes + delta;
  mo.retryable = false;
  std::uint64_t max = m.generated_tokens + m.remaining_budget;
  bool eos = false;
  if (m.payload.size() >= 1) eos = (m.payload[0] & 0x01) != 0;
  std::uint32_t eos_target = (m.payload.size() >= 2) ? m.payload[1] : 0;
  if ((eos && host_token == eos_target) || m.generated_tokens + 1 >= max) {
    mo.kind = MemberOutcomeKind::StepSuccessTerminal;
    mo.terminal = true;
  }
  pm.outcome = std::move(mo);
  return Result<PreparedMember>::ok(std::move(pm));
}

Result<PreparedDecode> CudaDecodeExecutor::prepare(const DecodeExecutionRequest& req) {
  std::lock_guard<std::mutex> lk(mu_);
  if (cudaSetDevice(cuda_device_) != cudaSuccess)
    return failed<PreparedDecode>(ErrorCode::BackendError, "cudaSetDevice failed");
  PreparedDecode out;
  out.dispatch_id = req.dispatch_id;
  out.epoch = req.epoch;
  out.worker = req.worker;
  out.worker_boot = req.worker_boot;
  Nanoseconds total = 0;
  for (const DecodeMemberSpec& m : req.members) {
    auto r = prepare_member(m, req, &total);
    if (!r.ok()) {
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

Result<MemberReceipt> CudaDecodeExecutor::commit(const CommitGrant& grant) {
  std::lock_guard<std::mutex> lk(mu_);
  if (cudaSetDevice(cuda_device_) != cudaSuccess)
    return failed<MemberReceipt>(ErrorCode::BackendError, "cudaSetDevice failed");

  // Idempotent duplicate commit.
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

  auto sit = states_.find(grant.state);
  if (sit == states_.end() || !sit->second.has_tentative || !sit->second.tentative) {
    return failed<MemberReceipt>(ErrorCode::UnknownState, "no prepared transition for grant state");
  }
  DevState& st = sit->second;
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
  if (st.tokens != grant.committed_position) {
    return failed<MemberReceipt>(ErrorCode::InvalidArgument, "grant committed-position mismatch");
  }

  // Verify the grant's digest binding by recomputing the committed pre-state
  // digest (the tentative post-state was verified at prepare time).
  std::vector<float> host_committed(kStateSize);
  if (cudaMemcpy(host_committed.data(), st.device, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
    return failed<MemberReceipt>(ErrorCode::BackendError, "cudaMemcpy committed failed");
  std::uint64_t pre_check = digest_dev(host_committed, st.tokens, st.kv_bytes);
  if (pre_check != grant.pre_state_digest || st.prepared_pre_digest != grant.pre_state_digest) {
    return failed<MemberReceipt>(ErrorCode::StateDigestMismatch, "grant pre-state digest mismatch");
  }
  if (st.prepared_post_digest != grant.post_state_digest) {
    return failed<MemberReceipt>(ErrorCode::StateDigestMismatch, "grant post-state digest mismatch");
  }

  // Atomically promote tentative -> committed.
  std::uint64_t before = st.tokens;
  std::uint64_t after = before + 1;
  if (cudaMemcpy(st.device, st.tentative, kStateSize * sizeof(float), cudaMemcpyDeviceToDevice) != cudaSuccess) {
    return failed<MemberReceipt>(ErrorCode::BackendError, "cudaMemcpy promote failed");
  }

  st.kv_bytes = st.tentative_kv_bytes;
  st.tokens = after;
  st.token = st.tentative_token;
  st.checksum = st.tentative_checksum;
  consumed_proposals_[grant.proposal] = true;
  clear_tentative(st);

  MemberReceipt rec;
  rec.receipt_id = ReceiptId::from(grant.grant_id.value());
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
  rec.pre_state_digest = grant.pre_state_digest;
  rec.post_state_digest = grant.post_state_digest;
  rec.delta_digest = grant.delta_digest;
  rec.outcome_kind = grant.outcome_kind;
  rec.terminal = grant.terminal;
  rec.token_identifier = grant.token_identifier;
  rec.active_ns = grant.active_ns;
  rec.committed_at = TimePoint(0);
  receipts_by_grant_[grant.grant_id] = rec;
  return Result<MemberReceipt>::ok(std::move(rec));
}

Result<void> CudaDecodeExecutor::abort(const AbortPrepared& abort) {
  std::lock_guard<std::mutex> lk(mu_);
  if (cudaSetDevice(cuda_device_) != cudaSuccess)
    return failed<void>(ErrorCode::BackendError, "cudaSetDevice failed");
  auto it = consumed_proposals_.find(abort.proposal);
  if (it != consumed_proposals_.end()) {
    return failed<void>(ErrorCode::AlreadyTerminal, "abort after commit");
  }
  auto sit = states_.find(abort.state);
  if (sit == states_.end()) return Result<void>::success();
  DevState& st = sit->second;
  if (st.has_tentative && st.prepared_proposal == abort.proposal) {
    clear_tentative(st);
  }
  return Result<void>::success();
}

std::uint64_t CudaDecodeExecutor::committed_state_digest(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  if (it == states_.end() || !it->second.inited) return 0;
  std::vector<float> host(kStateSize);
  if (cudaMemcpy(host.data(), it->second.device, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess) return 0;
  return digest_dev(host, it->second.tokens, it->second.kv_bytes);
}

bool CudaDecodeExecutor::has_prepared(StateId id, ProposalId proposal) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  return it->second.has_tentative && it->second.prepared_proposal == proposal;
}

std::uint64_t CudaDecodeExecutor::committed_tokens(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  return it == states_.end() ? 0 : it->second.tokens;
}

std::uint64_t CudaDecodeExecutor::dev_committed_checksum(StateId id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = states_.find(id);
  return it == states_.end() ? 0 : it->second.checksum;
}

}  // namespace decodefabric
