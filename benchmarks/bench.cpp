#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
using namespace decodefabric;

static DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t tenant, std::uint64_t budget) {
  DecodeRequest r; r.id = RequestId::from(id); r.initial_attempt = AttemptId::from(id);
  r.tenant = TenantId::from(tenant); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(seq); r.prompt_length = 4; r.max_generation_length = budget;
  r.tenant_weight = 1.0; r.state.id = StateId::from(seq); r.state.estimated_growth = 64;
  return r;
}

static double now_secs() {
  return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  std::vector<std::uint64_t> sizes = {1000, 10000, 100000};
  if (argc > 1) { sizes.clear(); for (int i = 1; i < argc; ++i) sizes.push_back(std::strtoull(argv[i], nullptr, 10)); }
  std::uint64_t budget = 8;
  for (std::uint64_t n : sizes) {
    DecodeFabric::Config cfg; cfg.group_limits.max_sequences = 4096;
    DecodeFabric fab(cfg);
    CpuDecodeExecutor ex(DeviceId::from(1));
    WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1); w.health = WorkerHealth::Healthy;
    w.device = ex.device(); w.advertised_capacity = n;
    CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1); k.backend = BackendKind::CPU; k.device = DeviceId::from(1); k.dtype = DType::F32;
    w.supported_models.push_back(k);
    (void)fab.register_worker(w);

    double t0 = now_secs();
    for (std::uint64_t i = 0; i < n; ++i) (void)fab.submit(req(i + 1, i + 1, (i % 4) + 1, budget));
    double t1 = now_secs();
    double submit_rate = (t1 - t0) > 0 ? static_cast<double>(n) / (t1 - t0) : 0.0;

    double t2 = now_secs();
    (void)fab.pump_until_idle(ex, TimePoint(0), -1);
    double t3 = now_secs();
    double elapsed = t3 - t2;
    Stats st = fab.stats();
    double sched_rate = elapsed > 0 ? static_cast<double>(st.decode_steps) / elapsed : 0.0;

    // Persistence serialization + recovery bench.
    double t4 = now_secs();
    auto bytes = fab.serialize_state();
    double t5 = now_secs();
    DecodeFabric::Config cfg2; DecodeFabric fab2(cfg2);
    auto r2 = fab2.recover_state(bytes.value());
    double t6 = now_secs();
    double serialize_us = bytes.ok() ? (t5 - t4) * 1e6 : 0.0;
    double recover_us = r2.ok() ? (t6 - t5) * 1e6 : 0.0;

    std::printf("bench n=%llu budget=%llu submit=%.0f/s schedule+complete=%.0f steps/s (gen=%llu) serialize=%.1fus recover=%.1fus\n",
                (unsigned long long)n, (unsigned long long)budget, submit_rate, sched_rate,
                (unsigned long long)st.generated_tokens, serialize_us, recover_us);
  }
  return 0;
}
