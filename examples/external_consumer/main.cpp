#include <decodefabric/fabric.hpp>
#include <decodefabric/cpu_executor.hpp>
#include <decodefabric/clock.hpp>
#include <cstdio>
using namespace decodefabric;
int main() {
  FixedClock clk;
  DecodeFabric::Config cfg; cfg.clock = &clk; cfg.group_limits.max_sequences = 32;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 256;
  CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1);
  k.backend = BackendKind::CPU; k.device = DeviceId::from(1); k.dtype = DType::F32;
  w.supported_models.push_back(k);
  (void)fab.register_worker(w);
  DecodeRequest r; r.id = RequestId::from(1); r.initial_attempt = AttemptId::from(1);
  r.tenant = TenantId::from(7); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(1); r.prompt_length = 4; r.max_generation_length = 5;
  r.tenant_weight = 1.0; r.state.id = StateId::from(1); r.state.estimated_growth = 64;
  AdmissionDecision d = fab.submit(r);
  if (!d.admitted) { std::printf("admission rejected: %s\n", d.reason.c_str()); return 1; }
  (void)fab.pump_until_idle(ex, clk.now(), -1);
  std::uint64_t gen = fab.stats().generated_tokens;
  std::printf("external consumer: admitted=%d generated=%llu active=%u\n", d.admitted ? 1 : 0,
              (unsigned long long)gen, fab.active_sequences());
  return (gen == 5 && fab.active_sequences() == 0) ? 0 : 2;
}
