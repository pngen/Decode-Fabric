#include "decodefabric/cpu_executor.hpp"
#include <chrono>
#include <cmath>
#include <cstring>

namespace decodefabric {

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

Result<DecodeExecutionResult> CpuDecodeExecutor::run_member(const DecodeMemberSpec& m,
                                                           Nanoseconds* active_ns) {
  auto t0 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(mu_);
  auto& st = states_[m.state.id];
  if (st.state.empty()) {
    std::uint64_t seed = m.generated_tokens ? 1 : 0;
    st.state = initial_state(m.state.id, seed + m.attempt.value());
    st.tokens = 0;
    st.kv_bytes = m.state.bytes_held ? m.state.bytes_held : 0;
    st.eos_enabled = false;
    st.eos_target = 0;
    if (m.payload.size() >= 1) { st.eos_enabled = (m.payload[0] & 0x01) != 0; }
    if (m.payload.size() >= 2) { st.eos_target = m.payload[1]; }
  }
  // Apply one authoritative numerical step: update state, produce a token.
  const std::size_t N = st.state.size();
  std::vector<double> next(N);
  for (std::size_t i = 0; i < N; ++i) {
    double acc = 0.0;
    double prev = st.state[(i + 1) % N];
    acc = 0.5 * st.state[i] + 0.25 * prev;
    next[i] = std::tanh(acc + 0.1 * std::sin(static_cast<double>(i)));
  }
  std::uint32_t token = step_token(next);
  // Renormalize (bounded). This is real numeric work whose exact values are
  // reproducible.
  double norm = 0.0;
  for (double v : next) norm += v * v;
  if (norm > 0.0) { double inv = 1.0 / std::sqrt(norm); for (double& v : next) v *= inv; }
  st.state = std::move(next);
  ++st.tokens;
  std::uint64_t delta = 64;  // per-token KV growth (bytes)
  st.kv_bytes += delta;

  auto t1 = std::chrono::steady_clock::now();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  *active_ns = static_cast<Nanoseconds>(ns);

  DecodeExecutionResult r;
  MemberOutcome mo;
  mo.sequence = m.sequence;
  mo.kind = MemberOutcomeKind::StepSuccessContinue;
  mo.generated = 1;
  mo.token_identifier = token;
  mo.terminal = false;
  mo.started_at = TimePoint(0);
  mo.finished_at = TimePoint(static_cast<Nanoseconds>(ns));
  mo.active_ns = static_cast<Nanoseconds>(ns);
  mo.kv_bytes_delta = delta;
  mo.kv_bytes_after = st.kv_bytes;
  mo.retryable = false;
  // Deterministic EOS-like terminal condition: the generated token equals the
  // configured EOS target AND the sequence opted in. Also budget exhaustion is
  // terminal (handled by the fabric as well).
  std::uint64_t max = m.generated_tokens + m.remaining_budget;
  if ((st.eos_enabled && token == st.eos_target) || (m.generated_tokens + 1 >= max)) {
    mo.kind = MemberOutcomeKind::StepSuccessTerminal;
    mo.terminal = true;
  }
  r.outcomes.push_back(std::move(mo));
  r.group_active_ns = *active_ns;
  r.group_error = ErrorCode::Ok;
  return Result<DecodeExecutionResult>::ok(std::move(r));
}

Result<DecodeExecutionResult> CpuDecodeExecutor::execute(const DecodeExecutionRequest& req) {
  DecodeExecutionResult out;
  out.dispatch_id = req.dispatch_id;
  out.epoch = req.epoch;
  out.worker = req.worker;
  out.worker_boot = req.worker_boot;
  Nanoseconds total = 0;
  for (const DecodeMemberSpec& m : req.members) {
    auto r = run_member(m, &total);
    if (!r.ok()) return r;
    for (MemberOutcome& mo : r.value().outcomes) {
      mo.attempt = m.attempt;
      mo.generation = m.generation;
      out.outcomes.push_back(std::move(mo));
    }
  }
  out.group_active_ns = total;
  out.group_error = ErrorCode::Ok;
  return Result<DecodeExecutionResult>::ok(std::move(out));
}

}  // namespace decodefabric
