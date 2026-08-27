#include "shared.hpp"
#include <cstdio>
using namespace decodefabric;
int main() {
  DecodeFabric::Config cfg; cfg.group_limits.max_sequences = 32;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 256;
  w.supported_models.push_back(ex::key());
  (void)fab.register_worker(w);
  // Tenant A weight 6 (two long seqs), Tenant B weight 1 (two seqs).
  (void)fab.submit(ex::req(40, 400, 7, 12, 6.0));
  (void)fab.submit(ex::req(41, 401, 7, 12, 6.0));
  (void)fab.submit(ex::req(42, 402, 9, 12, 1.0));
  (void)fab.submit(ex::req(43, 403, 9, 12, 1.0));
  (void)fab.pump_until_idle(ex, TimePoint(0), 1000);
  Snapshot sn = fab.snapshot();
  // Both tenants made progress (both completed all their tokens).
  std::printf("06 tenant_fairness: generated=%llu completed=%llu tenants_seen=%zu\n",
              (unsigned long long)sn.stats.generated_tokens, (unsigned long long)sn.completed, sn.per_tenant_tokens.size());
  bool a = false, b = false;
  for (const std::string& s : sn.per_tenant_tokens) { if (s.find("tenant=7") != std::string::npos) a = true; if (s.find("tenant=9") != std::string::npos) b = true; }
  return (a && b && sn.stats.generated_tokens == 48) ? 0 : 1;
}
