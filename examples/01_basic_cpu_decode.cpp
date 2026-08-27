#include "shared.hpp"
#include <cstdio>
using namespace decodefabric;
int main() {
  DecodeFabric::Config cfg;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 100;
  w.supported_models.push_back(ex::key());
  (void)fab.register_worker(w);
  AdmissionDecision d = fab.submit(ex::req(1, 1, 7, 5));
  (void)fab.pump_until_idle(ex, TimePoint(0), 50);
  std::printf("01 basic_cpu_decode: admitted=%d generated=%llu active=%u\n",
              d.admitted ? 1 : 0, (unsigned long long)fab.stats().generated_tokens, fab.active_sequences());
  return fab.stats().generated_tokens == 5 ? 0 : 1;
}
