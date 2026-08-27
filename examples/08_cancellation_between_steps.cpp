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
  (void)fab.submit(ex::req(60, 600, 7, 50));
  (void)fab.pump_once(ex, TimePoint(1000));  // one step
  CancelResult c = fab.cancel(SequenceId::from(600));
  (void)fab.pump_once(ex, TimePoint(2000));
  bool stopped = fab.sequence_state(SequenceId::from(600)) == SequenceState::Cancelled;
  std::printf("08 cancellation_between_steps: cancel=%d stopped=%d generated=%llu\n",
              c.cancelled ? 1 : 0, stopped ? 1 : 0, (unsigned long long)fab.stats().generated_tokens);
  return (c.cancelled && stopped) ? 0 : 1;
}
