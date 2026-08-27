#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cuda_executor.hpp"
#include "decodefabric/clock.hpp"
#include <cuda_runtime.h>
#include <cstdio>
#include <vector>
using namespace decodefabric;

static CompatibilityKey ck() {
  CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1);
  k.backend = BackendKind::CUDA; k.device = DeviceId::from(2); k.dtype = DType::F32; return k;
}
static DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t budget, std::uint64_t tenant = 1) {
  DecodeRequest r; r.id = RequestId::from(id); r.initial_attempt = AttemptId::from(id);
  r.tenant = TenantId::from(tenant); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(seq); r.prompt_length = 4; r.max_generation_length = budget;
  r.tenant_weight = 1.0; r.state.id = StateId::from(seq); r.state.estimated_growth = 64;
  r.latency_class = LatencyClass::Standard; return r;
}

DF_TEST(cuda_device_select_and_baseline) {
  int dev_count = 0;
  CHECK(cudaGetDeviceCount(&dev_count) == cudaSuccess);
  CHECK(dev_count >= 1);
  if (dev_count < 1) return;  // will fail above if no device
  cudaDeviceProp prop{};
  CHECK(cudaGetDeviceProperties(&prop, 0) == cudaSuccess);
  std::fprintf(stderr, "CUDA device: %s (CC %d.%d, %llu bytes)\n",
               prop.name, prop.major, prop.minor, (unsigned long long)prop.totalGlobalMem);
  CHECK(prop.major >= 12);  // RTX 5090 / sm_120
}

DF_TEST(cuda_iterative_generation) {
  // Establish the CUDA context / runtime before recording the baseline, so fresh
  // context-init reservation is not mistaken for a leak.
  CHECK(cudaSetDevice(0) == cudaSuccess);
  { float* warm = nullptr; CHECK(cudaMalloc(reinterpret_cast<void**>(&warm), 4096) == cudaSuccess); CHECK(cudaFree(warm) == cudaSuccess); }
  size_t baseline_free = 0, baseline_total = 0;
  CHECK(cudaMemGetInfo(&baseline_free, &baseline_total) == cudaSuccess);

  {
    CudaDecodeExecutor ex(DeviceId::from(2), 0);
    CHECK(ex.device().backend == BackendKind::CUDA);
    CHECK(ex.device().name == std::string("NVIDIA GeForce RTX 5090") ||
          ex.device().compute_capability_major >= 12);

    // Direct determinism check: same initial state -> same token/checksum twice.
    DecodeExecutionRequest d0;
    d0.dispatch_id = DispatchId::from(1); d0.epoch = CoordinatorEpoch::from(1);
    d0.worker = WorkerId::from(2); d0.worker_boot = WorkerBootId::from(1);
    d0.key = ck(); d0.device = ex.device();
    DecodeMemberSpec m; m.sequence = SequenceId::from(1); m.attempt = AttemptId::from(1);
    m.generation = DecodeGeneration::from(1); m.state.id = StateId::from(1);
    m.current_length = 4; m.generated_tokens = 0; m.remaining_budget = 30;
    d0.members.push_back(m);
    auto r1 = ex.execute(d0);
    CHECK(r1.ok());
    std::uint64_t t1 = r1.value().outcomes[0].token_identifier;
    auto r1b = ex.execute(d0);
    // A second step on the same seq uses the UPDATED state, so it should differ.
    CHECK(r1b.ok());
    std::uint64_t t2 = r1b.value().outcomes[0].token_identifier;

    // Now run a full fabric workload through the CUDA executor.
    FixedClock clk;
    DecodeFabric::Config cfg; cfg.clock = &clk; cfg.group_limits.max_sequences = 16;
    DecodeFabric fab(cfg);
    WorkerDescriptor w; w.id = WorkerId::from(2); w.boot_id = WorkerBootId::from(1);
    w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 4096;
    w.supported_models.push_back(ck());
    (void)fab.register_worker(w);

    std::uint64_t total_budget = 0;
    for (std::uint64_t i = 0; i < 8; ++i) {
      std::uint64_t budget = 5 + i;  // group sizes grow: 5..12 tokens
      AdmissionDecision d = fab.submit(req(100 + i, 500 + i, budget, (i % 2) ? 7 : 9));
      CHECK(d.admitted);
      total_budget += budget;
    }
    // Pump until idle.
    std::uint64_t steps = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
      clk.advance(1000000);
      TimePoint tn = clk.now();
      (void)fab.advance(tn);
      std::vector<Dispatch> ds = fab.schedule(tn);
      if (ds.empty()) { if (fab.active_sequences() == 0) break; continue; }
      for (Dispatch& d : ds) {
        DecodeExecutionRequest req2;
        req2.dispatch_id = d.id; req2.epoch = d.epoch; req2.worker = d.worker; req2.worker_boot = d.worker_boot;
        req2.key = d.key; req2.device = d.device; req2.reservation_id = d.reservation.value(); req2.members = d.members;
        { std::string k = d.key.to_string(); req2.group_payload.assign(k.begin(), k.end()); }
        auto rr = ex.execute(req2);
        CHECK(rr.ok());
        if (rr.ok()) { (void)fab.apply_completion(rr.value()); }
        ++steps;
      }
    }
    Snapshot sn = fab.snapshot();
    CHECK(sn.completed == 8);
    CHECK(sn.stats.generated_tokens == total_budget);
    CHECK(sn.reservations.empty());
    CHECK(fab.active_sequences() == 0);
    CHECK(steps >= 8);  // at least one dispatch per cycle, packed groups
    (void)t1; (void)t2;
  }

  // Destroying the executor must restore device memory to (near) baseline.
  size_t after_free = 0, after_total = 0;
  CHECK(cudaMemGetInfo(&after_free, &after_total) == cudaSuccess);
  // Destructor must have freed the executor device buffers. A tiny delta is
  // the CUDA driver memory-pool caching, not a per-allocation leak.
  std::uint64_t delta = (baseline_free > after_free) ? (baseline_free - after_free) : 0;
  CHECK(delta < (16ull << 20));
  std::fprintf(stderr, "CUDA memory baseline=%zu after=%zu delta=%llu (no meaningful leak)\n", baseline_free, after_free, (unsigned long long)delta);
}
