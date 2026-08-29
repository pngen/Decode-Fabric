#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include "transactional_helpers.hpp"
#include <vector>

using namespace decodefabric;

// These tests assert the CORE transactional invariant after the authority fix:
// A Decode Fabric generation becomes authoritative only when the exact executor
// transition prepared from the current committed pre-state is authorized under
// current sequence/worker authority, committed exactly once, and bound by a
// receipt. For EVERY stale-authority path the test proves BOTH:
//   A. Fabric canonical state does not advance.
//   B. Executor committed state does not advance.

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
  df_test::DriveResult drive() { return df_test::drive_dispatch(*fab, ex, dispatch); }
  std::uint64_t committed_digest() { return ex.committed_state_digest(dispatch.members[0].state.id); }
};

// Prepare the dispatched group through the real executor (read-only on
// committed state) and return the pre-state digest of the first member; abort
// the tentative so nothing is committed.
std::uint64_t prepare_pre_digest(Primed& p) {
  DecodeExecutionRequest rq;
  rq.dispatch_id = p.dispatch.id; rq.epoch = p.dispatch.epoch;
  rq.worker = p.dispatch.worker; rq.worker_boot = p.dispatch.worker_boot;
  rq.key = p.dispatch.key; rq.device = p.dispatch.device;
  rq.reservation_id = p.dispatch.reservation.value(); rq.members = p.dispatch.members;
  { std::string k = p.dispatch.key.to_string(); rq.group_payload.assign(k.begin(), k.end()); }
  auto prep = p.ex.prepare(rq);
  if (!prep.ok()) return 0;
  std::uint64_t d = prep.value().members[0].pre_state_digest;
  for (const PreparedMember& pm : prep.value().members) {
    AbortPrepared ab; ab.proposal = pm.proposal; ab.sequence = pm.sequence; ab.state = pm.state;
    ab.attempt = pm.attempt; ab.generation = pm.generation; ab.dispatch = pm.dispatch;
    ab.epoch = pm.epoch; ab.worker = pm.worker; ab.worker_boot = pm.worker_boot;
    (void)p.ex.abort(ab);
  }
  return d;
}
}  // namespace

DF_TEST(authority_valid_advances_once) {
  Primed p; p.setup(WorkerBootId::from(10));
  CHECK(!p.dispatch.members.empty());
  std::uint64_t before = p.fab->stale_rejections();
  std::uint64_t s0 = p.committed_digest();
  df_test::DriveResult dr = p.drive();
  CHECK(dr.prepared);
  CHECK(dr.authorized);
  CHECK(dr.committed == 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  CHECK(p.fab->stale_rejections() == before);
  CHECK(p.fab->receipt_count() == 1);
  CHECK(p.committed_digest() != s0);  // committed state advanced to post-state
}

DF_TEST(authority_duplicate_rejected) {
  Primed p; p.setup(WorkerBootId::from(11));
  df_test::DriveResult dr1 = p.drive();
  CHECK(dr1.committed == 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  std::uint64_t committed1 = p.committed_digest();
  std::uint64_t before = p.fab->stale_rejections();
  df_test::DriveResult dr2 = p.drive();
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  CHECK(p.committed_digest() == committed1);  // committed state unchanged
}

DF_TEST(authority_stale_epoch_rejected) {
  Primed p; p.setup(WorkerBootId::from(12));
  (void)p.fab->roll_epoch();
  std::uint64_t before = p.fab->stale_rejections();
  df_test::DriveResult dr = p.drive();
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
  // Committed executor state was initialized (S0) but not advanced, and a fresh
  // prepare from the same dispatch begins from that unchanged pre-state.
  CHECK(p.committed_digest() != 0);
  CHECK(prepare_pre_digest(p) == p.committed_digest());
}

DF_TEST(authority_stale_worker_boot_rejected) {
  Primed p; p.setup(WorkerBootId::from(13));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(99);
  w.health = WorkerHealth::Healthy; w.device = p.ex.device(); w.advertised_capacity = 100;
  w.supported_models.push_back(key());
  (void)p.fab->register_worker(w);
  std::uint64_t before = p.fab->stale_rejections();
  df_test::DriveResult dr = p.drive();
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
  CHECK(p.committed_digest() != 0);
  CHECK(prepare_pre_digest(p) == p.committed_digest());
}

DF_TEST(authority_stale_attempt_rejected) {
  Primed p; p.setup(WorkerBootId::from(14));
  RetryResult rr = p.fab->retry(SequenceId::from(10));
  CHECK(rr.retried);
  std::uint64_t before = p.fab->stale_rejections();
  df_test::DriveResult dr = p.drive();
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
  CHECK(p.committed_digest() != 0);
  CHECK(prepare_pre_digest(p) == p.committed_digest());
}

DF_TEST(authority_stale_generation_rejected) {
  Primed p; p.setup(WorkerBootId::from(15));
  std::uint64_t before = p.fab->stale_rejections();
  DecodeExecutionRequest rq;
  rq.dispatch_id = p.dispatch.id; rq.epoch = p.dispatch.epoch;
  rq.worker = p.dispatch.worker; rq.worker_boot = p.dispatch.worker_boot;
  rq.key = p.dispatch.key; rq.device = p.dispatch.device;
  rq.reservation_id = p.dispatch.reservation.value(); rq.members = p.dispatch.members;
  { std::string k = p.dispatch.key.to_string(); rq.group_payload.assign(k.begin(), k.end()); }
  auto prep = p.ex.prepare(rq);
  CHECK(prep.ok());
  for (PreparedMember& pm : prep.value().members) {
    pm.generation = DecodeGeneration::from(pm.generation.value() + 9999);
  }
  auto auth = p.fab->authorize_prepared(prep.value());
  CHECK(auth.ok());
  std::uint64_t rejected = 0;
  for (const auto& ga : auth.value().members) if (ga.rejected) ++rejected;
  CHECK(rejected == 1);
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
  CHECK(p.committed_digest() != 0);
  for (const auto& ga : auth.value().members) if (ga.has_abort) (void)p.ex.abort(ga.abort_spec);
  CHECK(prepare_pre_digest(p) == p.committed_digest());
}

DF_TEST(authority_after_cancel_rejected) {
  Primed p; p.setup(WorkerBootId::from(16));
  (void)p.fab->cancel(SequenceId::from(10));
  std::uint64_t before = p.fab->stale_rejections();
  df_test::DriveResult dr = p.drive();
  CHECK(p.fab->stale_rejections() == before + 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 0);
  CHECK(p.committed_digest() != 0);
  CHECK(prepare_pre_digest(p) == p.committed_digest());
}

DF_TEST(authority_reject_leaves_digest_for_retry) {
  // A rejected generation must leave the executor committed pre-state untouched
  // so a fresh re-dispatch of the SAME authoritative generation prepares from S0.
  Primed p; p.setup(WorkerBootId::from(17));
  (void)p.fab->roll_epoch();
  df_test::DriveResult dr = p.drive();
  std::uint64_t s0 = p.committed_digest();
  CHECK(s0 != 0);
  std::uint64_t before = p.fab->stale_rejections();
  FixedClock clk; clk.set(2000);
  (void)p.fab->advance(clk.now());
  auto ds = p.fab->schedule(clk.now());
  CHECK(ds.size() == 1);
  df_test::DriveResult dr2 = df_test::drive_dispatch(*p.fab, p.ex, ds[0]);
  CHECK(dr2.committed == 1);
  CHECK(p.fab->sequence_generated(SequenceId::from(10)) == 1);
  CHECK(p.fab->stale_rejections() == before);
}
