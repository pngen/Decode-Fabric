#include "shared.hpp"
#include <decodefabric/cuda_executor.hpp>
#include <cstdio>
using namespace decodefabric;
int main() {
  CudaDecodeExecutor ex(DeviceId::from(2), 0);
  if (ex.device().compute_capability_major < 12) { std::printf("11 cuda_decode: no sm_120 device\n"); return 2; }
  DecodeFabric::Config cfg; cfg.group_limits.max_sequences = 16;
  DecodeFabric fab(cfg);
  WorkerDescriptor w; w.id = WorkerId::from(2); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 4096;
  CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1);
  k.backend = BackendKind::CUDA; k.device = DeviceId::from(2); k.dtype = DType::F32;
  w.supported_models.push_back(k);
  (void)fab.register_worker(w);
  (void)fab.submit(ex::req(90, 90, 7, 8));
  (void)fab.pump_until_idle(ex, TimePoint(0), 100);
  std::printf("11 cuda_decode: device=%s cc=%d.%d generated=%llu active=%u\n",
              ex.device().name.c_str(), (int)ex.device().compute_capability_major,
              (int)ex.device().compute_capability_minor,
              (unsigned long long)fab.stats().generated_tokens, fab.active_sequences());
  return fab.stats().generated_tokens == 8 ? 0 : 1;
}
