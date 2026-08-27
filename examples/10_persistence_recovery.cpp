#include "shared.hpp"
#include <decodefabric/persistence.hpp>
#include <cstdio>
using namespace decodefabric;
int main() {
  FilePersistence fp("example_state_dir");
  {
    DecodeFabric::Config cfg;
    DecodeFabric fab(cfg);
    CpuDecodeExecutor ex(DeviceId::from(1));
    WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
    w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 100;
    w.supported_models.push_back(ex::key());
    (void)fab.register_worker(w);
    (void)fab.submit(ex::req(80, 80, 7, 10));
    (void)fab.pump_once(ex, TimePoint(1000));  // partial generation
    auto bytes = fab.serialize_state();
    if (bytes.ok()) (void)fp.write("state", bytes.value());
    std::printf("10 persistence_recovery: persisted %zu bytes, generated=%llu\n",
                bytes.ok() ? bytes.value().size() : 0, (unsigned long long)fab.stats().generated_tokens);
  }
  {
    DecodeFabric::Config cfg;
    DecodeFabric fab(cfg);
    auto b = fp.read("state");
    if (!b.ok()) { std::printf("10 persistence_recovery: no state\n"); return 1; }
    auto r = fab.recover_state(b.value());
    if (!r.ok()) { std::printf("10 persistence_recovery: recover failed: %s\n", r.error().message.c_str()); return 1; }
    std::printf("10 persistence_recovery: recovered active=%u generated=%llu\n",
                fab.active_sequences(), (unsigned long long)fab.stats().generated_tokens);
  }
  return 0;
}
