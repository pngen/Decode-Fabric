#include "test_framework.hpp"
#include "decodefabric/cpu_executor.hpp"
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
using namespace decodefabric;

// ---------------------------------------------------------------------------
// Deterministic per-run PRNG; the seed is reported so any failure is
// reproducible.
// ---------------------------------------------------------------------------
namespace {
std::uint64_t g_seed = 0;
std::uint64_t rnd_state() { return g_seed; }
std::uint64_t rnd() {
  g_seed ^= g_seed >> 12; g_seed ^= g_seed << 25; g_seed ^= g_seed >> 27;
  return g_seed * 0x2545F4914F6CDD1Dull;
}

DecodeExecutionRequest make_req(SequenceId seq, StateId state, AttemptId at,
                                DecodeGeneration gen, std::uint64_t budget,
                                std::uint64_t generated) {
  DecodeExecutionRequest r;
  r.dispatch_id = DispatchId::from(1);
  r.epoch = CoordinatorEpoch::from(1);
  r.worker = WorkerId::from(1);
  r.worker_boot = WorkerBootId::from(1);
  r.key.model = ModelId::from(1); r.key.revision = ModelRevision::from(1);
  r.key.backend = BackendKind::CPU; r.key.device = DeviceId::from(1);
  r.device.id = DeviceId::from(1);
  DecodeMemberSpec m;
  m.sequence = seq; m.attempt = at; m.generation = gen;
  m.state.id = state; m.state.estimated_growth = 64;
  m.current_length = 4 + generated; m.generated_tokens = generated;
  m.remaining_budget = budget;
  r.members.push_back(m);
  return r;
}

AbortPrepared ab_from(ProposalId proposal, StateId state, SequenceId sequence,
                       AttemptId at, DecodeGeneration gen, DispatchId disp,
                       CoordinatorEpoch ep, WorkerId wk, WorkerBootId wb) {
  AbortPrepared ab;
  ab.proposal = proposal; ab.state = state; ab.sequence = sequence; ab.attempt = at;
  ab.generation = gen; ab.dispatch = disp; ab.epoch = ep; ab.worker = wk; ab.worker_boot = wb;
  return ab;
}

CommitGrant grant_from(const PreparedMember& pm, GrantId gid) {
  CommitGrant g;
  g.grant_id = gid; g.proposal = pm.proposal;
  g.epoch = pm.epoch; g.worker = pm.worker; g.worker_boot = pm.worker_boot;
  g.sequence = pm.sequence; g.state = pm.state; g.attempt = pm.attempt;
  g.generation = pm.generation; g.dispatch = pm.dispatch;
  g.committed_position = pm.committed_position_before;
  g.pre_state_digest = pm.pre_state_digest; g.post_state_digest = pm.post_state_digest;
  g.delta_digest = pm.delta_digest;
  g.outcome_kind = pm.outcome.kind; g.terminal = pm.outcome.terminal;
  g.token_identifier = static_cast<std::uint32_t>(pm.outcome.token_identifier);
  g.active_ns = pm.active_ns;
  return g;
}
}  // namespace

// ---------------------------------------------------------------------------
// 1) Randomized transactional state-machine property proof.
//    For one sequence, repeatedly prepare then randomly ACCEPT (commit) or
//    REJECT (abort). Every outcome must satisfy the invariants:
//      - a rejected (aborted) transition NEVER changes the committed digest
//      - an accepted commit advances the committed digest by exactly one step
//      - the committed executor generation never exceeds the fabric's
//        authoritative generated count
//      - receipt(G).post == prepare(G+1).pre (receipt-chain continuity)
//      - at most one receipt per generation, and no grant consumed twice
//      - the committed digest is stable across a duplicate prepare of the same
//        authoritative generation
// ---------------------------------------------------------------------------
DF_TEST(inv_randomized_transactional_state_machine) {
  g_seed = 0x12345678abcdefull;
  const std::uint64_t kSeed = rnd_state();
  const std::uint64_t kOps = 3000;
  CpuDecodeExecutor ex(DeviceId::from(1));
  const StateId state = StateId::from(1);
  const SequenceId seq = SequenceId::from(1);
  const AttemptId attempt = AttemptId::from(1);
  const std::uint64_t budget = 50;

  std::uint64_t generated = 0;          // fabric authoritative committed count
  std::uint64_t committed_digest = 0;   // fabric expected committed-state digest
  bool digest_est = false;
  std::uint64_t receipts = 0;
  std::uint64_t rejects = 0;
  std::uint64_t commits = 0;
  std::uint64_t checks = 0;
  DecodeGeneration gen = DecodeGeneration::from(1);

  for (std::uint64_t op = 0; op < kOps; ++op) {
    DecodeExecutionRequest req = make_req(seq, state, attempt, gen, budget, generated);
    auto p = ex.prepare(req);
    ++checks;
    // A prepare must never change the committed executor state.
    if (digest_est) {
      ++checks;
      CHECK(ex.committed_state_digest(state) == committed_digest);
    }
    CHECK(p.ok());
    ++checks;
    if (!p.ok()) continue;

    std::uint64_t r2 = rnd();
    const bool accept = (r2 % 2 == 0);
    const PreparedMember& pm = p.value().members[0];

    if (!accept) {
      // REJECT: abort; committed digest and fabric count unchanged.
      AbortPrepared ab = ab_from(pm.proposal, state, seq, attempt, gen, DispatchId::from(1),
                                 CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1));
      auto ar = ex.abort(ab);
      ++checks; CHECK(ar.ok());
      if (!digest_est) {
        ++checks; CHECK(ex.committed_state_digest(state) == pm.pre_state_digest);
      } else {
        ++checks; CHECK(ex.committed_state_digest(state) == committed_digest);
      }
      // A later re-prepare of the SAME authoritative generation derives the same
      // pre-state digest (the committed state was not changed).
      auto p2 = ex.prepare(req);
      ++checks; CHECK(p2.ok());
      if (p2.ok()) {
        ++checks; CHECK(p2.value().members[0].pre_state_digest == pm.pre_state_digest);
        (void)ex.abort(ab_from(p2.value().members[0].proposal, state, seq, attempt, gen,
                               DispatchId::from(1), CoordinatorEpoch::from(1),
                               WorkerId::from(1), WorkerBootId::from(1)));
      }
      ++rejects;
      ++checks; CHECK(ex.committed_tokens(state) == generated);
      continue;
    }

    // ACCEPT: verify the pre-state digest matches the fabric's expectation
    // (receipt-chain continuity), then commit exactly once.
    if (digest_est) {
      ++checks; CHECK(pm.pre_state_digest == committed_digest);
    } else {
      ++checks; CHECK(ex.committed_state_digest(state) == pm.pre_state_digest);
    }
    CommitGrant g = grant_from(pm, GrantId::from(100000 + op));
    auto cr = ex.commit(g);
    ++checks; CHECK(cr.ok());
    if (!cr.ok()) continue;
    MemberReceipt rec = cr.value();
    ++commits; ++receipts;
    ++generated;
    ++checks; CHECK(rec.committed_position_before == generated - 1);
    ++checks; CHECK(rec.committed_position_after == generated);
    ++checks; CHECK(ex.committed_state_digest(state) == rec.post_state_digest);
    ++checks; CHECK(ex.committed_tokens(state) == generated);
    committed_digest = rec.post_state_digest;
    digest_est = true;
    // Duplicate commit of a consumed grant is idempotent (no double apply).
    auto dup = ex.commit(g);
    ++checks; CHECK(dup.ok());
    ++checks; CHECK(dup.value().receipt_id == rec.receipt_id);
    ++checks; CHECK(ex.committed_tokens(state) == generated);  // unchanged (no double)
    gen = DecodeGeneration::from(gen.value() + 1);
  }

  ++checks; CHECK(receipts == commits);
  ++checks; CHECK(ex.committed_tokens(state) == generated);
  std::printf("[inv] random seed=%llu ops=%llu checks=%llu commits=%llu rejects=%llu receipts=%llu gen=%llu committed_tokens=%llu\n",
              (unsigned long long)kSeed, (unsigned long long)kOps, (unsigned long long)checks,
              (unsigned long long)commits, (unsigned long long)rejects, (unsigned long long)receipts,
              (unsigned long long)generated, (unsigned long long)ex.committed_tokens(state));
}

// ---------------------------------------------------------------------------
// 2) Concurrency: many threads commit the SAME one-use grant concurrently.
//    No double commit, no race, committed token count advances once, and the
//    test completes (i.e. no deadlock on the executor's mutex).
// ---------------------------------------------------------------------------
DF_TEST(inv_concurrent_single_commit_no_double) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  const StateId state = StateId::from(1);
  const SequenceId seq = SequenceId::from(1);
  const AttemptId attempt = AttemptId::from(1);
  const DecodeGeneration gen = DecodeGeneration::from(1);
  DecodeExecutionRequest req = make_req(seq, state, attempt, gen, 10, 0);
  auto p = ex.prepare(req);
  CHECK(p.ok());
  CommitGrant g = grant_from(p.value().members[0], GrantId::from(999));

  const int kThreads = 8;
  std::atomic<int> ok{0};
  std::atomic<int> err{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      auto cr = ex.commit(g);
      if (cr.ok()) ++ok; else ++err;
    });
  }
  for (auto& t : threads) t.join();
  CHECK(ex.committed_tokens(state) == 1);
  CHECK(ok.load() + err.load() == kThreads);
  std::printf("[inv] concurrency threads=%d ok=%d err=%d committed_tokens=%llu\n",
              kThreads, ok.load(), err.load(), (unsigned long long)ex.committed_tokens(state));
}

// ---------------------------------------------------------------------------
// 3) Terminal sequence: preparing a budget-exhausted generation reports a
//    terminal outcome; the fabric gate rejects any further advance at the
//    finalize/authorize boundary (asserted in authority_test).
// ---------------------------------------------------------------------------
DF_TEST(inv_terminal_cannot_commit) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  StateId state = StateId::from(1);
  DecodeExecutionRequest req = make_req(SequenceId::from(1), state, AttemptId::from(1),
                                        DecodeGeneration::from(1), 1, 0);
  auto p = ex.prepare(req);
  CHECK(p.ok());
  CommitGrant g = grant_from(p.value().members[0], GrantId::from(1));
  CHECK(ex.commit(g).ok());
  CHECK(ex.committed_tokens(state) == 1);
  DecodeExecutionRequest req2 = make_req(SequenceId::from(1), state, AttemptId::from(1),
                                         DecodeGeneration::from(2), 1, 1);
  auto p2 = ex.prepare(req2);
  CHECK(p2.ok());
  CHECK(p2.value().members[0].outcome.terminal == true);
  (void)ex.abort(ab_from(p2.value().members[0].proposal, state, SequenceId::from(1),
                         AttemptId::from(1), DecodeGeneration::from(2), DispatchId::from(1),
                         CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1)));
  CHECK(ex.committed_tokens(state) == 1);
}
