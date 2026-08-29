#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cuda_executor.hpp"
#include "decodefabric/clock.hpp"
#include "transactional_helpers.hpp"
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

// Build a one-use grant from a prepared member and commit it, returning the
// deterministic receipt.
static MemberReceipt commit_grant(CudaDecodeExecutor& ex, const PreparedMember& pm) {
  CommitGrant g;
  g.grant_id = GrantId::from(100000 + pm.committed_position_after + pm.state.value());
  g.proposal = pm.proposal; g.epoch = pm.epoch; g.worker = pm.worker;
  g.worker_boot = pm.worker_boot; g.sequence = pm.sequence; g.state = pm.state;
  g.attempt = pm.attempt; g.generation = pm.generation; g.dispatch = pm.dispatch;
  g.committed_position = pm.committed_position_before;
  g.pre_state_digest = pm.pre_state_digest; g.post_state_digest = pm.post_state_digest;
  g.delta_digest = pm.delta_digest;
  g.outcome_kind = pm.outcome.kind; g.terminal = pm.outcome.terminal;
  g.token_identifier = static_cast<std::uint32_t>(pm.outcome.token_identifier);
  g.active_ns = pm.active_ns;
  auto cr = ex.commit(g);
  CHECK(cr.ok());
  return cr.ok() ? cr.value() : MemberReceipt{};
}

static DecodeExecutionRequest basic_req(CudaDecodeExecutor& ex) {
  DecodeExecutionRequest d0;
  d0.dispatch_id = DispatchId::from(1); d0.epoch = CoordinatorEpoch::from(1);
  d0.worker = WorkerId::from(2); d0.worker_boot = WorkerBootId::from(1);
  d0.key = ck(); d0.device = ex.device();
  DecodeMemberSpec m; m.sequence = SequenceId::from(1); m.attempt = AttemptId::from(1);
  m.generation = DecodeGeneration::from(1); m.state.id = StateId::from(1);
  m.current_length = 4; m.generated_tokens = 0; m.remaining_budget = 30;
  d0.members.push_back(m);
  return d0;
}

DF_TEST(cuda_device_select_and_baseline) {
  int dev_count = 0;
  CHECK(cudaGetDeviceCount(&dev_count) == cudaSuccess);
  CHECK(dev_count >= 1);
  if (dev_count < 1) return;
  cudaDeviceProp prop{};
  CHECK(cudaGetDeviceProperties(&prop, 0) == cudaSuccess);
  std::fprintf(stderr, "CUDA device: %s (CC %d.%d, %llu bytes)\n",
               prop.name, prop.major, prop.minor, (unsigned long long)prop.totalGlobalMem);
  CHECK(prop.major >= 12);  // RTX 5090 / sm_120
}

DF_TEST(cuda_iterative_generation) {
  CHECK(cudaSetDevice(0) == cudaSuccess);
  { float* warm = nullptr; CHECK(cudaMalloc(reinterpret_cast<void**>(&warm), 4096) == cudaSuccess); CHECK(cudaFree(warm) == cudaSuccess); }
  size_t baseline_free = 0, baseline_total = 0;
  CHECK(cudaMemGetInfo(&baseline_free, &baseline_total) == cudaSuccess);

  {
    CudaDecodeExecutor ex(DeviceId::from(2), 0);
    CHECK(ex.device().backend == BackendKind::CUDA);
    CHECK(ex.device().name == std::string("NVIDIA GeForce RTX 5090") ||
          ex.device().compute_capability_major >= 12);

    // Determinism + committed/tentative separation: prepare does NOT mutate the
    // committed device state. A rejected prepare leaves the committed digest at
    // S0; commit advances it to S1; the next step prepares from S1.
    DecodeExecutionRequest d0 = basic_req(ex);
    std::uint64_t s0 = ex.committed_state_digest(StateId::from(1));
    CHECK(s0 == 0);  // not yet initialized

    auto p1 = ex.prepare(d0);
    CHECK(p1.ok());
    // Prepare is read-only on the COMMITTED state: it initializes S0 but does
    // NOT advance it to S1. The committed digest must equal the prepared
    // pre-state digest (S0), not the post-state digest (S1).
    CHECK(ex.committed_state_digest(StateId::from(1)) == p1.value().members[0].pre_state_digest);
    std::uint64_t t1 = p1.value().members[0].outcome.token_identifier;

    // Abort the tentative: committed digest must remain the unchanged pre-state S0.
    AbortPrepared ab1;
    ab1.proposal = p1.value().members[0].proposal;
    ab1.sequence = p1.value().members[0].sequence; ab1.state = StateId::from(1);
    ab1.attempt = p1.value().members[0].attempt; ab1.generation = p1.value().members[0].generation;
    ab1.dispatch = d0.dispatch_id; ab1.epoch = d0.epoch; ab1.worker = d0.worker; ab1.worker_boot = d0.worker_boot;
    (void)ex.abort(ab1);
    CHECK(ex.committed_state_digest(StateId::from(1)) == p1.value().members[0].pre_state_digest);

    // Re-prepare from the unchanged committed pre-state and commit.
    auto p1b = ex.prepare(d0);
    CHECK(p1b.ok());
    CHECK(p1b.value().members[0].pre_state_digest == p1.value().members[0].pre_state_digest);
    MemberReceipt r1 = commit_grant(ex, p1b.value().members[0]);
    CHECK(ex.committed_state_digest(StateId::from(1)) != 0);  // now S1

    // Next generation prepares from S1 (the receipt's post-state).
    DecodeExecutionRequest d1 = d0;
    d1.members[0].generation = DecodeGeneration::from(2);
    d1.members[0].generated_tokens = 1;
    auto p2 = ex.prepare(d1);
    CHECK(p2.ok());
    CHECK(p2.value().members[0].pre_state_digest == r1.post_state_digest);
    std::uint64_t t2 = p2.value().members[0].outcome.token_identifier;
    (void)t1; (void)t2;

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
      std::uint64_t budget = 5 + i;
      AdmissionDecision d = fab.submit(req(100 + i, 500 + i, budget, (i % 2) ? 7 : 9));
      CHECK(d.admitted);
      total_budget += budget;
    }
    std::uint64_t steps = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
      clk.advance(1000000);
      TimePoint tn = clk.now();
      (void)fab.advance(tn);
      std::vector<Dispatch> ds = fab.schedule(tn);
      if (ds.empty()) { if (fab.active_sequences() == 0) break; continue; }
      for (Dispatch& d : ds) {
        (void)df_test::drive_dispatch(fab, ex, d);
        ++steps;
      }
    }
    Snapshot sn = fab.snapshot();
    CHECK(sn.completed == 8);
    CHECK(sn.stats.generated_tokens == total_budget);
    CHECK(sn.reservations.empty());
    CHECK(fab.active_sequences() == 0);
    CHECK(steps >= 8);
  }

  size_t after_free = 0, after_total = 0;
  CHECK(cudaMemGetInfo(&after_free, &after_total) == cudaSuccess);
  std::uint64_t delta = (baseline_free > after_free) ? (baseline_free - after_free) : 0;
  CHECK(delta < (16ull << 20));
  std::fprintf(stderr, "CUDA memory baseline=%zu after=%zu delta=%llu (no meaningful leak)\n", baseline_free, after_free, (unsigned long long)delta);
}
