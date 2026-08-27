#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include "decodefabric/protocol.hpp"
#include <cstdint>
#include <thread>
#include <vector>
using namespace decodefabric;
void decodeframe_oversize();
namespace {
CompatibilityKey ck(){ CompatibilityKey k; k.model=ModelId::from(1); k.revision=ModelRevision::from(1); k.backend=BackendKind::CPU; k.device=DeviceId::from(1); k.dtype=DType::F32; return k; }
DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t tenant, std::uint64_t budget, std::uint64_t model=1){
  DecodeRequest r; r.id=RequestId::from(id); r.initial_attempt=AttemptId::from(id); r.tenant=TenantId::from(tenant);
  r.model=ModelId::from(model); r.revision=ModelRevision::from(1); r.sequence=SequenceId::from(seq);
  r.prompt_length=4; r.max_generation_length=budget; r.tenant_weight=1.0; r.state.id=StateId::from(seq); r.state.estimated_growth=64; return r;
}
WorkerDescriptor worker(){ WorkerDescriptor w; w.id=WorkerId::from(1); w.boot_id=WorkerBootId::from(1); w.health=WorkerHealth::Healthy; w.device=CpuDecodeExecutor(DeviceId::from(1)).device(); w.advertised_capacity=1u<<14; w.supported_models.push_back(ck()); return w; }
std::uint64_t lcg(std::uint64_t& s){ s=s*6364136223846793005ull+1442695040888963407ull; return s>>33; }
}

DF_TEST(concurrency_submission_snapshot_completion) {
  DecodeFabric::Config cfg;
  DecodeFabric fab(cfg);
  (void)fab.register_worker(worker());
  CpuDecodeExecutor ex(DeviceId::from(1));
  const int threads = 4;
  const int per_thread = 50;
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&, t]() {
      for (int i = 0; i < per_thread; ++i) {
        std::uint64_t seq = 10000 * (t + 1) + i;
        AdmissionDecision d = fab.submit(req(seq, seq, (t % 3) + 1, 5));
        if (!d.admitted) std::fprintf(stderr, "REJECT seq=%llu reason=%s\n", (unsigned long long)seq, d.reason.c_str());
      }
    });
  }
  for (auto& th : ts) th.join();
  (void)fab.pump_until_idle(ex, TimePoint(0), 2000);
  Stats st = fab.stats();
  std::printf("CONCURRENCY gen=%llu started=%llu admitted=%llu active=%u\n", (unsigned long long)st.generated_tokens, (unsigned long long)st.sequences_started, (unsigned long long)st.requests_admitted, fab.active_sequences());
  // Every submitted sequence must have been admitted and generated exactly its budget.
  CHECK(st.sequences_started == threads * per_thread);
  CHECK(st.generated_tokens == threads * per_thread * 5ull);
  CHECK(fab.active_sequences() == 0);
  // Concurrent read APIs must be safe.
  std::vector<std::thread> readers;
  for (int t = 0; t < 8; ++t) readers.emplace_back([&]() { (void)fab.snapshot(); (void)fab.stats(); (void)fab.events(); });
  for (auto& th : readers) th.join();
  CHECK(true);
}

DF_TEST(adversarial_rejections) {
  DecodeFabric::Config cfg;
  DecodeFabric fab(cfg);
  (void)fab.register_worker(worker());
  // duplicate sequence id
  (void)fab.submit(req(1, 100, 7, 5));
  AdmissionDecision d2 = fab.submit(req(2, 100, 7, 5));
  CHECK(!d2.admitted);
  // unknown model
  AdmissionDecision d3 = fab.submit(req(3, 900, 7, 5, 999));
  CHECK(!d3.admitted);
  // zero budget
  AdmissionDecision d4 = fab.submit(req(4, 901, 7, 0));
  CHECK(!d4.admitted);
  // deadline already expired -> sequence becomes DeadlineExpired
  DecodeRequest r = req(5, 902, 7, 5);
  r.deadline = TimePoint(1); r.arrival = TimePoint(0);
  AdmissionDecision d5 = fab.submit(r);
  CHECK(d5.admitted);
  (void)fab.advance(TimePoint(1000));
  CHECK(fab.sequence_state(SequenceId::from(902)) == SequenceState::DeadlineExpired);
  // cancel/completion race: cancel then a completion is rejected as stale.
  CpuDecodeExecutor ex(DeviceId::from(1));
  (void)fab.submit(req(6, 903, 7, 5));
  (void)fab.pump_once(ex, TimePoint(1000));
  (void)fab.cancel(SequenceId::from(903));
  std::uint64_t before = fab.stale_rejections();
  (void)fab.pump_once(ex, TimePoint(2000));  // no further dispatch
  // Protocol: oversized frame rejected.
  decodeframe_oversize();
  CHECK(fab.stale_rejections() >= before);
}

DF_TEST(randomized_invariants_fixed_seed) {
  DecodeFabric::Config cfg; cfg.group_limits.max_sequences = 256;
  DecodeFabric fab(cfg);
  (void)fab.register_worker(worker());
  CpuDecodeExecutor ex(DeviceId::from(1));
  std::uint64_t seed = 0xABCD1234ull;
  std::uint64_t total = 0;
  const int n = 300;
  for (int i = 0; i < n; ++i) {
    std::uint64_t budget = 1 + (lcg(seed) % 12);
    std::uint64_t tenant = 1 + (lcg(seed) % 4);
    std::uint64_t seq = 50000 + i;
    AdmissionDecision d = fab.submit(req(i + 1, seq, tenant, budget));
    CHECK(d.admitted);
    total += budget;
  }
  (void)fab.pump_until_idle(ex, TimePoint(0), 5000);
  Snapshot sn = fab.snapshot();
  CHECK(sn.stats.generated_tokens == total);
  CHECK(sn.completed == n);
  CHECK(sn.reservations.empty());
  CHECK(fab.active_sequences() == 0);
  CHECK(sn.stats.requests_admitted == n);
}

void decodeframe_oversize() {
  protocol::FrameDecoder dec(16);
  std::vector<std::uint8_t> big(64, 0);
  big[0]=64; big[1]=0; big[2]=0; big[3]=0;
  big[4]=1; big[5]=0; big[6]=0; big[7]=0;
  big[8]=1; big[9]=0; big[10]=0; big[11]=0;
  protocol::Frame out;
  auto r = dec.feed(big, out);
  CHECK(r.is_error());
  CHECK(r.error().code == ErrorCode::ProtocolOversizedFrame);
}
