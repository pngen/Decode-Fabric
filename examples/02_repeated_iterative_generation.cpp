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
  (void)fab.submit(ex::req(2, 2, 7, 20));
  (void)fab.pump_until_idle(ex, TimePoint(0), 200);
  std::uint64_t gen = fab.stats().generated_tokens;
  std::printf("02 repeated_iterative_generation: generated=%llu steps=%llu (monotonic per step)\n",
              (unsigned long long)gen, (unsigned long long)fab.stats().decode_steps);
  return (gen == 20 && fab.stats().decode_steps == 20) ? 0 : 1;
}
