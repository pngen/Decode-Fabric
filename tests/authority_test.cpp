#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include <vector>

using namespace decodefabric;

namespace {
CompatibilityKey key() {
  CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1);
  k.backend = BackendKind::CPU; k.device = DeviceId::from(1); k.dtype = DType::F32; return k;
}
DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t budget) {
  DecodeRequest r; r.id = RequestId::from(id); r.initial_attempt = AttemptId::from(1);
  r.tenant = TenantId::from(1); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(seq); r.prompt_length = 4; r.max_generation_length = budget;
  r.tenant_weight = 1.0; r.latency_class = LatencyClass::Standard; r.priority = 0;
  r.state.id = StateId::from(seq); r.state.generation = 0; r.state.estimated_growth = 64; return r;
}
// Register a worker and submit a long sequence; run one schedule to obtain a
// dispatch (which sets the sequence's in-flight authority).
struct Primed {
  std::unique_ptr<DecodeFabric> fab;
  CpuDecodeExecutor ex;
  Dispatch dispatch;
  WorkerBootId boot;
  Primed() : ex(DeviceId::from(1)) {}
  void setup(WorkerBootId wb, std::uint64_t budget = 20) {
    fab = std::make_unique<DecodeFabric>([] { DecodeFabric::Config c; c.group_limits.max_sequences = 8; return c; }());
    WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = wb; w.health = WorkerHealth::Healthy;
    w.device = ex.device(); w.advertised_capacity = 100; w.supported_models.push_back(key());
    (void)fab->register_worker(w);
    (void)fab->submit(req(1, 10, budget));
    FixedClock clk; clk.set(1000);
    (void)fab->advance(clk.now());
    auto ds = fab->schedule(clk.now());
    CHECK(ds.size() == 1);
    if (!ds.empty()) { dispatch = ds[0]; }
    boot = wb;
  }
  DecodeExecutionResult base_result(std::uint64_t epoch, WorkerBootId b, std::uint64_t attempt, std::uint64_t gen) {
    DecodeExecutionResult r;
    r.dispatch_id = dispatch.id; r.epoch = CoordinatorEpoch::from(epoch);
    r.worker = dispatch.worker; r.worker_boot = b;
    MemberOutcome mo;
    mo.sequence = dispatch.members[0].sequence;
    mo.kind = MemberOutcomeKind::StepSuccessContinue;
    mo.generated = 1; mo.attempt = AttemptId::from(attempt); mo.generation = DecodeGeneration::from(gen);
    mo.finished_at = TimePoint(2000); mo.active_ns = 100;
    r.outcomes.push_back(mo);
    return r;
  }
};
}  // namespace

DF_TEST(authority_valid_advances_once) {
  Primed p; p.setup(WorkerBootId::from(10));
  CHECK(!p.dispatch.members.empty());
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), p.boot, p.dispatch.members[0].attempt.value(), p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  CHECK(p.fab->stale_rejections() == before);
}

DF_TEST(authority_duplicate_rejected) {
  Primed p; p.setup(WorkerBootId::from(11));
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), p.boot, p.dispatch.members[0].attempt.value(), p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  std::uint64_t before = p.fab->stale_rejections();
  // Replay the same authoritative completion.
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);  // no advance
}

DF_TEST(authority_stale_epoch_rejected) {
  Primed p; p.setup(WorkerBootId::from(12));
  // Roll the epoch so the sequence's in-flight epoch becomes stale.
  (void)p.fab->roll_epoch();
  std::uint64_t before = p.fab->stale_rejections();
  std::uint64_t old_epoch = p.dispatch.epoch.value();
  DecodeExecutionResult r = p.base_result(old_epoch, p.boot, p.dispatch.members[0].attempt.value(), p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);  // never advanced
}

DF_TEST(authority_stale_worker_boot_rejected) {
  Primed p; p.setup(WorkerBootId::from(13));
  // The worker restarts with a new boot id.
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(99);
  w.health = WorkerHealth::Healthy; w.device = p.ex.device(); w.advertised_capacity = 100;
  w.supported_models.push_back(key());
  (void)p.fab->register_worker(w);
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), WorkerBootId::from(13), p.dispatch.members[0].attempt.value(), p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
}

DF_TEST(authority_stale_attempt_rejected) {
  Primed p; p.setup(WorkerBootId::from(14));
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), p.boot, 7777, p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
}

DF_TEST(authority_stale_generation_rejected) {
  Primed p; p.setup(WorkerBootId::from(15));
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), p.boot, p.dispatch.members[0].attempt.value(), 0);
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
}

DF_TEST(authority_after_cancel_rejected) {
  Primed p; p.setup(WorkerBootId::from(16));
  (void)p.fab->cancel(SequenceId::from(10));
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionResult r = p.base_result(p.fab->epoch().value(), p.boot, p.dispatch.members[0].attempt.value(), p.dispatch.members[0].generation.value());
  (void)p.fab->apply_completion(r);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
}
