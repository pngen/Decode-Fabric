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
  auto pump = [&]() {
    std::vector<Dispatch> ds = fab.schedule(TimePoint(static_cast<Nanoseconds>(1000)));
    for (Dispatch& d : ds) ex::drive(fab, ex, d);
  };
  (void)fab.submit(ex::req(20, 20, 7, 10));  // long-lived
  (void)fab.submit(ex::req(21, 21, 7, 3));   // short, compatible => can join later
  for (int i = 0; i < 100 && fab.active_sequences() > 0; ++i) pump();
  std::printf("04 group_growth_shrink: active=%u gen=%llu (continuous membership change)\n", fab.active_sequences(), (unsigned long long)fab.stats().generated_tokens);
  return fab.stats().generated_tokens == 13 ? 0 : 1;
}
