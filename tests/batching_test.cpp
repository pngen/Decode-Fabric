#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include <cstdlib>
#include <memory>
#include <vector>

using namespace decodefabric;

namespace {
CompatibilityKey make_key() {
  CompatibilityKey k;
  k.model = ModelId::from(1);
  k.revision = ModelRevision::from(1);
  k.backend = BackendKind::CPU;
  k.device = DeviceId::from(1);
  k.dtype = DType::F32;
  return k;
}
DecodeRequest make_req(const char* id, std::uint64_t seq, std::uint64_t tenant,
                       std::uint64_t max_gen, double weight = 1.0) {
  DecodeRequest r;
  r.id = RequestId::from(std::strtoull(id, nullptr, 10));
  r.initial_attempt = AttemptId::from(1);
  r.tenant = TenantId::from(tenant);
  r.model = ModelId::from(1);
  r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(seq);
  r.prompt_length = 4;
  r.max_generation_length = max_gen;
  r.tenant_weight = weight;
  r.latency_class = LatencyClass::Standard;
  r.priority = 0;
  r.state.id = StateId::from(seq);
  r.state.generation = 0;
  r.state.estimated_growth = 64;
  return r;
}
}  // namespace

// Runs cycles of schedule+execute+apply until no runnable work remains. Returns
// the sequence of dispatch member-counts across cycles.
static std::vector<std::size_t> run_cycles(DecodeFabric& fab, CpuDecodeExecutor& ex,
                                           FixedClock& clk, int max_cycles) {
  std::vector<std::size_t> sizes;
  for (int i = 0; i < max_cycles; ++i) {
    clk.advance(1000000);
    TimePoint tn = clk.now();
    (void)fab.advance(tn);
    std::vector<Dispatch> ds = fab.schedule(tn);
    if (ds.empty()) {
      // Check if anything remains ready; if not, we're done.
      if (fab.ready_sequences() == 0 && fab.active_sequences() == 0) break;
      continue;
    }
    for (Dispatch& d : ds) {
      DecodeExecutionRequest req;
      req.dispatch_id = d.id; req.epoch = d.epoch; req.worker = d.worker;
      req.worker_boot = d.worker_boot; req.key = d.key; req.device = d.device;
      req.reservation_id = d.reservation.value(); req.members = d.members;
      { std::string k = d.key.to_string(); req.group_payload.assign(k.begin(), k.end()); }
      auto rr = ex.execute(req);
      if (rr.ok()) (void)fab.apply_completion(rr.value());
      sizes.push_back(d.members.size());
    }
  }
  return sizes;
}

DF_TEST(continuous_batching_membership_changes) {
  FixedClock clk(0);
  DecodeFabric::Config cfg;
  cfg.clock = &clk;
  cfg.group_limits.max_sequences = 16;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w;
  w.id = WorkerId::from(1);
  w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy;
  w.device = ex.device();
  w.advertised_capacity = 100;
  w.supported_models.push_back(make_key());
  (void)fab.register_worker(w);

  (void)fab.submit(make_req("1", 100, 7, 6));
  (void)fab.submit(make_req("2", 101, 7, 8));
  (void)fab.submit(make_req("3", 102, 7, 10));

  // A new compatible sequence joins a later iteration.
  std::vector<std::size_t> sizes;
  int cycles = 0;
  bool joined = false;
  while (cycles < 60) {
    ++cycles;
    clk.advance(1000000);
    TimePoint tn = clk.now();
    (void)fab.advance(tn);
    if (cycles == 3 && !joined) { (void)fab.submit(make_req("4", 103, 7, 12)); joined = true; }

    std::vector<Dispatch> ds = fab.schedule(tn);

    for (Dispatch& d : ds) {
      DecodeExecutionRequest req;
      req.dispatch_id = d.id; req.epoch = d.epoch; req.worker = d.worker;
      req.worker_boot = d.worker_boot; req.key = d.key; req.device = d.device;
      req.reservation_id = d.reservation.value(); req.members = d.members;
      { std::string k = d.key.to_string(); req.group_payload.assign(k.begin(), k.end()); }
      auto rr = ex.execute(req);
      if (rr.ok()) (void)fab.apply_completion(rr.value());
      sizes.push_back(d.members.size());
    }
    if (fab.active_sequences() == 0) break;
  }
  CHECK(cycles < 40);
  CHECK(joined);
  // Group membership changed across iterations (not a single static batch).
  std::size_t distinct = 0;
  for (std::size_t i = 1; i < sizes.size(); ++i) if (sizes[i] != sizes[i-1]) ++distinct;
  CHECK(distinct > 0);
  // All sequences completed; total generated tokens equals sum of budgets.
  Snapshot sn = fab.snapshot();
  std::uint64_t total = sn.stats.generated_tokens;
  CHECK(total == 6 + 8 + 10 + 12);
  CHECK(sn.completed == 4);
  CHECK(fab.active_sequences() == 0);
  CHECK(sn.reservations.empty());
  CHECK(sn.stats.requests_admitted == 4);
}

DF_TEST(budgets_and_fairness_across_tenants) {
  FixedClock clk(0);
  DecodeFabric::Config cfg;
  cfg.clock = &clk;
  cfg.group_limits.max_sequences = 24;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device();
  w.advertised_capacity = 200; w.supported_models.push_back(make_key());
  (void)fab.register_worker(w);

  // Tenant A (weight 6), Tenant B (weight 1). Two long sequences each.
  (void)fab.submit(make_req("101", 200, 10, 5, 6.0));
  (void)fab.submit(make_req("102", 201, 10, 5, 6.0));
  (void)fab.submit(make_req("201", 202, 30, 5, 1.0));
  (void)fab.submit(make_req("202", 203, 30, 5, 1.0));
  auto sizes = run_cycles(fab, ex, clk, 200);
  CHECK(sizes.size() >= 4);
  Snapshot sn = fab.snapshot();
  CHECK(sn.stats.generated_tokens == 4 * 5);
  CHECK(sn.completed == 4);
  // Both tenants made progress.
  bool a_seen = false, b_seen = false;
  for (const std::string& s : sn.per_tenant_tokens) {
    if (s.find("tenant=10") != std::string::npos) a_seen = true;
    if (s.find("tenant=30") != std::string::npos) b_seen = true;
  }
  CHECK(a_seen && b_seen);
}

DF_TEST(cancellation_between_steps) {
  FixedClock clk(0);
  DecodeFabric::Config cfg;
  cfg.clock = &clk;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device();
  w.advertised_capacity = 100; w.supported_models.push_back(make_key());
  (void)fab.register_worker(w);
  (void)fab.submit(make_req("301", 300, 7, 50));
  clk.advance(1000000);
  TimePoint tn = clk.now();
  (void)fab.advance(tn);
  std::vector<Dispatch> ds = fab.schedule(tn);
  CHECK(ds.size() == 1);
  DecodeExecutionRequest req; req.dispatch_id = ds[0].id; req.epoch = ds[0].epoch;
  req.worker = ds[0].worker; req.worker_boot = ds[0].worker_boot; req.key = ds[0].key;
  req.device = ds[0].device; req.reservation_id = ds[0].reservation.value(); req.members = ds[0].members;
  { std::string k = ds[0].key.to_string(); req.group_payload.assign(k.begin(), k.end()); }
  auto r = ex.execute(req);
  CHECK(r.ok());
  (void)fab.apply_completion(r.value());
  CHECK(fab.active_sequences() == 1);
  // Now cancel between steps (sequence is ReadyForNextToken).
  CancelResult c = fab.cancel(SequenceId::from(300));
  CHECK(c.cancelled);
  Snapshot sn = fab.snapshot();
  CHECK(sn.cancelled == 1);
  // Must not generate further tokens.
  (void)fab.advance(clk.now());
  ds = fab.schedule(clk.now());
  CHECK(ds.empty());
}

DF_TEST(no_work_submitted_invalid) {
  FixedClock clk(0);
  DecodeFabric::Config cfg; cfg.clock = &clk;
  DecodeFabric fab(cfg);
  DecodeRequest r;
  AdmissionDecision d = fab.submit(r);
  CHECK(!d.admitted);
}
