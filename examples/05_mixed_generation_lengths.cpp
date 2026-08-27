#include "shared.hpp"
#include <cstdio>
using namespace decodefabric;
int main() {
  DecodeFabric::Config cfg;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 256;
  w.supported_models.push_back(ex::key());
  (void)fab.register_worker(w);
  std::uint64_t total = 0;
  for (std::uint64_t i = 0; i < 5; ++i) { std::uint64_t b = 2 + 2 * i; (void)fab.submit(ex::req(30 + i, 300 + i, 7, b)); total += b; }
  (void)fab.pump_until_idle(ex, TimePoint(0), 500);
  std::printf("05 mixed_generation_lengths: expected=%llu generated=%llu active=%u\n",
              (unsigned long long)total, (unsigned long long)fab.stats().generated_tokens, fab.active_sequences());
  return fab.stats().generated_tokens == total ? 0 : 1;
}
