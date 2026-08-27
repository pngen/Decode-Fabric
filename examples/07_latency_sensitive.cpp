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
  // Latency-sensitive request (Interactive, per-token target) vs a bulk one.
  DecodeRequest a = ex::req(50, 500, 7, 10);
  a.latency_class = LatencyClass::Interactive; a.per_token_target_ns = 1000000;
  (void)fab.submit(a);
  (void)fab.submit(ex::req(51, 501, 9, 10));
  (void)fab.pump_until_idle(ex, TimePoint(0), 500);
  Explain e = fab.explain(SequenceId::from(500), "why_waiting");
  std::printf("07 latency_sensitive: generated=%llu explain=%s\n",
              (unsigned long long)fab.stats().generated_tokens, e.answer.c_str());
  return fab.stats().generated_tokens == 20 ? 0 : 1;
}
