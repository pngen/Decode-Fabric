#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include <vector>
using namespace decodefabric;
namespace {
CompatibilityKey ck(){ CompatibilityKey k; k.model=ModelId::from(1); k.revision=ModelRevision::from(1); k.backend=BackendKind::CPU; k.device=DeviceId::from(1); k.dtype=DType::F32; return k; }
DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t budget, std::uint64_t tenant=1){ DecodeRequest r; r.id=RequestId::from(id); r.initial_attempt=AttemptId::from(id); r.tenant=TenantId::from(tenant); r.model=ModelId::from(1); r.revision=ModelRevision::from(1); r.sequence=SequenceId::from(seq); r.prompt_length=4; r.max_generation_length=budget; r.tenant_weight=1.0; r.state.id=StateId::from(seq); r.state.estimated_growth=64; r.latency_class=LatencyClass::Standard; return r; }
void setup(DecodeFabric& fab){ CpuDecodeExecutor ex(DeviceId::from(1)); WorkerDescriptor w; w.id=WorkerId::from(1); w.boot_id=WorkerBootId::from(1); w.health=WorkerHealth::Healthy; w.device=ex.device(); w.advertised_capacity=100; w.supported_models.push_back(ck()); (void)fab.register_worker(w); }
}

DF_TEST(persistence_serialize_magic_and_roundtrip) {
  FixedClock clk;
  { DecodeFabric::Config cfg; cfg.clock = &clk; DecodeFabric fab(cfg); setup(fab);
    (void)fab.submit(req(1, 100, 10)); (void)fab.submit(req(2, 101, 10));
    clk.advance(1000000); (void)fab.pump_once(*new CpuDecodeExecutor(DeviceId::from(1)), clk.now());
    clk.advance(1000000); (void)fab.pump_once(*new CpuDecodeExecutor(DeviceId::from(1)), clk.now());
    auto bytes = fab.serialize_state();
    CHECK(bytes.ok());
    CHECK(bytes.value().size() >= 4);
    CHECK(bytes.value()[0] == 'D' && bytes.value()[1] == 'F' && bytes.value()[2] == 'S' && bytes.value()[3] == 'T');
    std::uint64_t before_active = fab.active_sequences();
    std::uint64_t before_gen = fab.stats().generated_tokens;
    // Recover into a fresh fabric.
    { DecodeFabric::Config cfg2; cfg2.clock = &clk; DecodeFabric fab2(cfg2);
      auto r = fab2.recover_state(bytes.value());
      CHECK(r.ok());
      CHECK(fab2.active_sequences() == before_active);
      CHECK(fab2.stats().generated_tokens == before_gen);
    }
  }
}

DF_TEST(persistence_rejects_corrupt_checksum_and_truncation) {
  FixedClock clk;
  DecodeFabric::Config cfg; cfg.clock = &clk; DecodeFabric fab(cfg); setup(fab);
  (void)fab.submit(req(1, 100, 10));
  clk.advance(1000000); (void)fab.pump_once(*new CpuDecodeExecutor(DeviceId::from(1)), clk.now());
  auto bytes = fab.serialize_state();
  CHECK(bytes.ok());
  // Truncate (remove the trailing checksum + some data).
  std::vector<std::uint8_t> truncated(bytes.value().begin(), bytes.value().begin() + bytes.value().size() - 5);
  { DecodeFabric::Config c2; c2.clock = &clk; DecodeFabric fab2(c2); CHECK(fab2.recover_state(truncated).is_error()); }
}
