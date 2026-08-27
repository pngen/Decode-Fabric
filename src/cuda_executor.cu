#include "decodefabric/cuda_executor.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <cstring>

namespace decodefabric {

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
    std::uint64_t bits; std::memcpy(&bits, &din, sizeof(bits));
    for (int i = 0; i < 8; ++i) { h ^= (bits >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
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

Result<DecodeExecutionResult> CudaDecodeExecutor::run_member(const DecodeMemberSpec& m,
                                                            Nanoseconds* active_ns) {
  auto t0 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(mu_);
  cudaError_t err = cudaSetDevice(cuda_device_);
  if (err != cudaSuccess)
    return failed<DecodeExecutionResult>(ErrorCode::BackendError,
                                         "cudaSetDevice failed: " + std::string(cudaGetErrorString(err)));

  auto& st = states_[m.state.id];
  if (!st.inited) {
    cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&st.device), kStateSize * sizeof(float));
    if (e != cudaSuccess) return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMalloc failed");
    std::vector<float> init = initial_state(m.state.id, m.attempt.value());
    e = cudaMemcpy(st.device, init.data(), kStateSize * sizeof(float), cudaMemcpyHostToDevice);
    if (e != cudaSuccess) return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMemcpy init failed");
    st.kv_bytes = m.state.bytes_held ? m.state.bytes_held : 0;
    st.token = 0; st.checksum = 0;
    st.inited = true;
  }

  // A real GPU allocation for result copies.
  std::uint32_t* d_token = nullptr; std::uint64_t* d_cksum = nullptr; float* d_out = nullptr;
  if (cudaMalloc(reinterpret_cast<void**>(&d_token), sizeof(std::uint32_t)) != cudaSuccess)
    return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMalloc token failed");
  if (cudaMalloc(reinterpret_cast<void**>(&d_cksum), sizeof(std::uint64_t)) != cudaSuccess)
    return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMalloc cksum failed");
  if (cudaMalloc(reinterpret_cast<void**>(&d_out), kStateSize * sizeof(float)) != cudaSuccess)
    return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMalloc out failed");

  df_decode_step_kernel<<<1, kStateSize>>>(st.device, d_out, d_token, d_cksum, static_cast<int>(kStateSize));
  cudaError_t ke = cudaGetLastError();
  if (ke != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "kernel launch failed: " + std::string(cudaGetErrorString(ke))); }
  cudaError_t se = cudaDeviceSynchronize();
  if (se != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaDeviceSynchronize failed: " + std::string(cudaGetErrorString(se))); }

  std::vector<float> host_out(kStateSize);
  std::uint32_t host_token = 0; std::uint64_t host_cksum = 0;
  if (cudaMemcpy(host_out.data(), d_out, kStateSize * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMemcpy out failed"); }
  if (cudaMemcpy(&host_token, d_token, sizeof(std::uint32_t), cudaMemcpyDeviceToHost) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMemcpy token failed"); }
  if (cudaMemcpy(&host_cksum, d_cksum, sizeof(std::uint64_t), cudaMemcpyDeviceToHost) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMemcpy cksum failed"); }

  // Stash the new state back into the persistent device buffer.
  if (cudaMemcpy(st.device, host_out.data(), kStateSize * sizeof(float), cudaMemcpyHostToDevice) != cudaSuccess) { cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out); return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaMemcpy state failed"); }

  st.token = host_token; st.checksum = host_cksum;
  cudaFree(d_token); cudaFree(d_cksum); cudaFree(d_out);
  std::uint64_t delta = 64;
  st.kv_bytes += delta;

  auto t1 = std::chrono::steady_clock::now();
  Nanoseconds ns = static_cast<Nanoseconds>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  *active_ns = ns;

  DecodeExecutionResult r;
  MemberOutcome mo;
  mo.sequence = m.sequence;
  mo.kind = MemberOutcomeKind::StepSuccessContinue;
  mo.generated = 1;
  mo.token_identifier = st.token;
  mo.terminal = false;
  mo.finished_at = TimePoint(ns);
  mo.active_ns = ns;
  mo.kv_bytes_delta = delta;
  mo.kv_bytes_after = st.kv_bytes;
  mo.retryable = false;
  std::uint64_t max = m.generated_tokens + m.remaining_budget;
  bool eos = false;
  if (m.payload.size() >= 1) eos = (m.payload[0] & 0x01) != 0;
  std::uint32_t eos_target = (m.payload.size() >= 2) ? m.payload[1] : 0;
  if ((eos && st.token == eos_target) || m.generated_tokens + 1 >= max) {
    mo.kind = MemberOutcomeKind::StepSuccessTerminal;
    mo.terminal = true;
  }
  r.outcomes.push_back(std::move(mo));
  r.group_active_ns = ns;
  return Result<DecodeExecutionResult>::ok(std::move(r));
}

Result<DecodeExecutionResult> CudaDecodeExecutor::execute(const DecodeExecutionRequest& req) {
  DecodeExecutionResult out;
  out.dispatch_id = req.dispatch_id;
  out.epoch = req.epoch;
  out.worker = req.worker;
  out.worker_boot = req.worker_boot;
  Nanoseconds total = 0;
  if (cudaSetDevice(cuda_device_) != cudaSuccess)
    return failed<DecodeExecutionResult>(ErrorCode::BackendError, "cudaSetDevice failed");
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
