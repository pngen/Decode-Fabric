#include "test_framework.hpp"
#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include "transactional_helpers.hpp"
#include <vector>
using namespace decodefabric;

// ---------------------------------------------------------------------------
// Recovery / idempotency hardening: an accepted generation is itself the
// durable, idempotent receipt of that exact authorized transition. If fabric
// acceptance exists but executor promotion was not observed, recovery
// reconciles that SAME generation exactly once and never authorizes a second
// transition. Duplicate/replayed acceptance cannot cause a second executor
// promotion; recovery is exactly-once and receipt-chain continuity is kept.
// ---------------------------------------------------------------------------

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

// A fabric + worker + one submitted/scheduled sequence, driven to accept a
// generation and returning the single-member receipt so it can be replayed.
struct Primed {
  std::unique_ptr<DecodeFabric> fab;
  CpuDecodeExecutor ex;
  Dispatch dispatch;
  FixedClock clk;
  Primed() : ex(DeviceId::from(1)) {}
  void setup(std::uint64_t seq, std::uint64_t budget) {
    clk.set(1000);
    DecodeFabric::Config cfg; cfg.clock = &clk; cfg.group_limits.max_sequences = 8;
    fab = std::make_unique<DecodeFabric>(cfg);
    WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
    w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 100;
    w.supported_models.push_back(key());
    (void)fab->register_worker(w);
    (void)fab->submit(req(1, seq, budget));
    (void)fab->advance(clk.now());
    auto ds = fab->schedule(clk.now());
    CHECK(ds.size() == 1);
    if (!ds.empty()) dispatch = ds[0];
  }
  // Drive one dispatch (prepare -> authorize -> commit -> apply_commit_receipt)
  // and return the receipt for the dispatched member.
  MemberReceipt drive_and_receipt() {
    DecodeExecutionRequest q;
    q.dispatch_id = dispatch.id; q.epoch = dispatch.epoch; q.worker = dispatch.worker;
    q.worker_boot = dispatch.worker_boot; q.key = dispatch.key; q.device = dispatch.device;
    q.reservation_id = dispatch.reservation.value(); q.members = dispatch.members;
    { std::string k = dispatch.key.to_string(); q.group_payload.assign(k.begin(), k.end()); }
    auto prep = ex.prepare(q);
    CHECK(prep.ok());
    auto auth = fab->authorize_prepared(prep.value());
    CHECK(auth.ok());
    MemberReceipt ret;
    ReceiptDecode rd; rd.dispatch_id = dispatch.id; rd.epoch = dispatch.epoch;
    rd.worker = dispatch.worker; rd.worker_boot = dispatch.worker_boot;
    for (const auto& ga : auth.value().members) {
      if (ga.has_grant) {
        auto cr = ex.commit(ga.grant);
        CHECK(cr.ok());
        if (cr.ok()) { MemberReceipt mr = cr.value(); mr.committed_at = TimePoint(0); rd.receipts.push_back(mr); ret = mr; }
      } else if (ga.has_abort) { (void)ex.abort(ga.abort_spec); }
    }
    (void)fab->apply_commit_receipt(rd);
    return ret;
  }
};

// Replay a single receipt through the fabric (idempotency check).
void replay_receipt(DecodeFabric& fab, const MemberReceipt& r) {
  ReceiptDecode rd;
  rd.dispatch_id = r.dispatch; rd.epoch = r.epoch; rd.worker = r.worker;
  rd.worker_boot = r.worker_boot;
  rd.receipts.push_back(r);
  (void)fab.apply_commit_receipt(rd);
}
}  // namespace

DF_TEST(rec_acceptance_durable_and_replay_no_second_promote) {
  const SequenceId seq = SequenceId::from(101);
  Primed p; p.setup(seq.value(), 3);
  MemberReceipt r1 = p.drive_and_receipt();
  std::uint64_t committed_after_accept = p.ex.committed_tokens(StateId::from(101));
  CHECK(committed_after_accept == 1);
  CHECK(p.fab->sequence_generated(seq) == 1);
  CHECK(p.fab->has_accepted_generation(seq));
  AcceptedGeneration acc = p.fab->accepted_generation(seq);
  CHECK(acc.idempotency_key != 0);
  CHECK(acc.sequence == seq);
  CHECK(acc.generation.value() == 1);
  CHECK(acc.post_state_digest == r1.post_state_digest);
  CHECK(acc.promotion_observed == true);

  // Replay the SAME acceptance: idempotent, no second finalize/promotion.
  replay_receipt(*p.fab, r1);
  CHECK(p.fab->sequence_generated(seq) == 1);
  CHECK(p.ex.committed_tokens(StateId::from(101)) == 1);  // no double promotion

  // Persist + recover into a fresh fabric and a fresh executor.
  auto bytes = p.fab->serialize_state();
  CHECK(bytes.ok());
  FixedClock clk2; clk2.set(2000);
  DecodeFabric::Config cfg2; cfg2.clock = &clk2; cfg2.group_limits.max_sequences = 8;
  DecodeFabric fab2(cfg2);
  CHECK(fab2.recover_state(bytes.value()).ok());
  CHECK(fab2.has_accepted_generation(seq));
  AcceptedGeneration acc2 = fab2.accepted_generation(seq);
  CHECK(acc2.idempotency_key == acc.idempotency_key);
  CHECK(acc2.post_state_digest == acc.post_state_digest);
  CHECK(fab2.sequence_generated(seq) == 1);
  // Receipt-chain continuity: recovered expected committed-state digest is the
  // accepted generation's post digest.
  CHECK(fab2.sequence_committed_digest(seq) == acc.post_state_digest);

  // Replay identical acceptance into the recovered fabric: idempotent.
  replay_receipt(fab2, r1);
  CHECK(fab2.sequence_generated(seq) == 1);

  // Re-authorize the SAME already-accepted generation: it must be reconciled
  // (rejected, no second grant) and cannot promote a second transition.
  CpuDecodeExecutor ex2(DeviceId::from(1));
  DecodeExecutionRequest q;
  q.dispatch_id = p.dispatch.id; q.epoch = p.dispatch.epoch; q.worker = p.dispatch.worker;
  q.worker_boot = p.dispatch.worker_boot; q.key = p.dispatch.key; q.device = p.dispatch.device;
  q.reservation_id = p.dispatch.reservation.value(); q.members = p.dispatch.members;
  { std::string k = p.dispatch.key.to_string(); q.group_payload.assign(k.begin(), k.end()); }
  auto prep = ex2.prepare(q);
  CHECK(prep.ok());
  auto auth = fab2.authorize_prepared(prep.value());
  CHECK(auth.ok());
  std::uint64_t rejected = 0, granted = 0;
  for (const auto& ga : auth.value().members) {
    if (ga.rejected || ga.aborted) ++rejected;
    if (ga.has_grant) ++granted;
  }
  CHECK(rejected == 1);
  CHECK(granted == 0);  // no second commit grant
  CHECK(fab2.sequence_generated(seq) == 1);
  CHECK(ex2.committed_tokens(StateId::from(101)) == 0);  // executor did not promote twice
}

DF_TEST(rec_terminal_generation_no_repromote) {
  const SequenceId seq = SequenceId::from(202);
  Primed p; p.setup(seq.value(), 1);
  MemberReceipt r1 = p.drive_and_receipt();   // budget 1 -> terminal after gen 1
  CHECK(p.fab->sequence_generated(seq) == 1);
  CHECK(p.fab->sequence_state(seq) == SequenceState::Completed);
  CHECK(p.fab->has_accepted_generation(seq));
  AcceptedGeneration acc = p.fab->accepted_generation(seq);
  CHECK(acc.terminal == true);

  // Persist + recover; the terminal accepted generation is preserved. No further
  // dispatch exists (sequence is terminal), so the promotion is implicitly
  // confirmed; a replay must not re-promote.
  auto bytes = p.fab->serialize_state();
  CHECK(bytes.ok());
  FixedClock clk2; clk2.set(3000);
  DecodeFabric::Config cfg2; cfg2.clock = &clk2;
  DecodeFabric fab2(cfg2);
  CHECK(fab2.recover_state(bytes.value()).ok());
  CHECK(fab2.active_sequences() == 0);
  CHECK(fab2.sequence_state(seq) == SequenceState::Completed);
  CHECK(fab2.has_accepted_generation(seq));
  CHECK(fab2.accepted_generation(seq).terminal == true);

  // Replay the terminal receipt into the recovered fabric: idempotent, no
  // second promotion and no resurrection.
  replay_receipt(fab2, r1);
  CHECK(fab2.active_sequences() == 0);
  CHECK(fab2.sequence_state(seq) == SequenceState::Completed);
  CHECK(fab2.sequence_generated(seq) == 1);
  // No new dispatch for a terminal sequence.
  auto ds2 = fab2.schedule(clk2.now());
  CHECK(ds2.empty());
}
