#include "shared.hpp"
#include <cstdio>
#include <vector>
using namespace decodefabric;
int main() {
  DecodeFabric::Config cfg; cfg.group_limits.max_sequences = 32;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 256;
  w.supported_models.push_back(ex::key());
  (void)fab.register_worker(w);
  (void)fab.submit(ex::req(10, 10, 7, 3));
  (void)fab.submit(ex::req(11, 11, 7, 5));
  (void)fab.submit(ex::req(12, 12, 7, 7));
  std::vector<std::size_t> sizes;
  for (int cyc = 0; cyc < 100; ++cyc) {
    std::vector<Dispatch> ds = fab.schedule(TimePoint(static_cast<Nanoseconds>(cyc * 1000)));
    for (Dispatch& d : ds) {
      DecodeExecutionRequest q;
      q.dispatch_id = d.id; q.epoch = d.epoch; q.worker = d.worker; q.worker_boot = d.worker_boot;
      q.key = d.key; q.device = d.device; q.reservation_id = d.reservation.value(); q.members = d.members;
      { std::string k = d.key.to_string(); q.group_payload.assign(k.begin(), k.end()); }
      auto r = ex.execute(q);
      if (r.ok()) (void)fab.apply_completion(r.value());
      sizes.push_back(d.members.size());
    }
    if (fab.active_sequences() == 0) break;
  }
  std::uint64_t total = 0;
  for (std::size_t s : sizes) total += s;
  std::printf("03 continuous_batching: dispatches=%zu group-member-sum=%llu done active=%u\n",
              sizes.size(), (unsigned long long)total, fab.active_sequences());
  return (fab.stats().generated_tokens == 3 + 5 + 7) ? 0 : 1;
}
