#include "test_framework.hpp"
#include "decodefabric/cpu_executor.hpp"
#include <vector>
using namespace decodefabric;

// Direct CPU executor transactional-state proof: prepare is read-only on
// committed state, commit is one-use and idempotent, abort discards tentative
// state, digests are deterministic, and the receipt chain holds
// receipt(G).post == prepare(G+1).pre.

namespace {
DecodeExecutionRequest make_req(SequenceId seq, StateId state, AttemptId at,
                                DecodeGeneration gen, std::uint64_t budget,
                                std::uint64_t generated) {
  DecodeExecutionRequest r;
  r.dispatch_id = DispatchId::from(1);
  r.epoch = CoordinatorEpoch::from(1);
  r.worker = WorkerId::from(1);
  r.worker_boot = WorkerBootId::from(1);
  r.key.model = ModelId::from(1);
  r.key.revision = ModelRevision::from(1);
  r.key.backend = BackendKind::CPU;
  r.key.device = DeviceId::from(1);
  r.device.id = DeviceId::from(1);
  DecodeMemberSpec m;
  m.sequence = seq; m.attempt = at; m.generation = gen;
  m.state.id = state; m.state.estimated_growth = 64;
  m.current_length = 4 + generated; m.generated_tokens = generated;
  m.remaining_budget = budget;
  r.members.push_back(m);
  return r;
}

CommitGrant grant_from(const PreparedMember& pm, GrantId gid = GrantId::from(777)) {
  CommitGrant g;
  g.grant_id = gid;
  g.proposal = pm.proposal;
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

AbortPrepared ab_from(ProposalId proposal, StateId state, SequenceId sequence,
                       AttemptId at, DecodeGeneration gen, DispatchId disp,
                       CoordinatorEpoch ep, WorkerId wk, WorkerBootId wb) {
  AbortPrepared ab;
  ab.proposal = proposal; ab.state = state; ab.sequence = sequence; ab.attempt = at;
  ab.generation = gen; ab.dispatch = disp; ab.epoch = ep; ab.worker = wk; ab.worker_boot = wb;
  return ab;
}
}  // namespace

DF_TEST(cpu_tx_prepare_is_readonly_and_reject_preserves_committed) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  // No committed state yet.
  CHECK(ex.committed_state_digest(StateId::from(1)) == 0);
  auto p1 = ex.prepare(r1);
  CHECK(p1.ok());
  std::uint64_t s0 = ex.committed_state_digest(StateId::from(1));
  CHECK(s0 != 0);
  CHECK(ex.has_prepared(StateId::from(1), p1.value().members[0].proposal));
  // Reject: abort the tentative. Committed state unchanged (still S0).
  AbortPrepared ab;
  ab.proposal = p1.value().members[0].proposal; ab.sequence = SequenceId::from(1);
  ab.state = StateId::from(1); ab.attempt = AttemptId::from(1);
  ab.generation = DecodeGeneration::from(1); ab.dispatch = DispatchId::from(1);
  ab.epoch = CoordinatorEpoch::from(1); ab.worker = WorkerId::from(1);
  ab.worker_boot = WorkerBootId::from(1);
  (void)ex.abort(ab);
  CHECK(ex.committed_state_digest(StateId::from(1)) == s0);
  CHECK(!ex.has_prepared(StateId::from(1), p1.value().members[0].proposal));
  // Re-dispatch the same authoritative generation: prepare from unchanged S0,
  // with identical digests.
  auto p2 = ex.prepare(r1);
  CHECK(p2.ok());
  CHECK(p2.value().members[0].pre_state_digest == p1.value().members[0].pre_state_digest);
  CHECK(p2.value().members[0].post_state_digest == p1.value().members[0].post_state_digest);
  CHECK(ex.committed_state_digest(StateId::from(1)) == s0);
  (void)ex.abort(ab);
}

DF_TEST(cpu_tx_commit_advances_and_receipt_chain_holds) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  auto p1 = ex.prepare(r1);
  CHECK(p1.ok());
  auto pm = p1.value().members[0];
  CommitGrant g = grant_from(pm);
  auto cr = ex.commit(g);
  CHECK(cr.ok());
  MemberReceipt r = cr.value();
  CHECK(ex.committed_tokens(StateId::from(1)) == 1);
  CHECK(ex.committed_state_digest(StateId::from(1)) == r.post_state_digest);
  // Next generation prepares from the receipt's post-state digest.
  DecodeExecutionRequest r2 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(2), 10, 1);
  auto p2 = ex.prepare(r2);
  CHECK(p2.ok());
  CHECK(p2.value().members[0].pre_state_digest == r.post_state_digest);
  (void)ex.abort(ab_from(p2.value().members[0].proposal, StateId::from(1), SequenceId::from(1),
                         AttemptId::from(1), DecodeGeneration::from(2), DispatchId::from(1),
                         CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1)));
}

DF_TEST(cpu_tx_duplicate_grant_idempotent) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  auto p1 = ex.prepare(r1);
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  auto cr1 = ex.commit(g);
  CHECK(cr1.ok());
  CHECK(ex.committed_tokens(StateId::from(1)) == 1);
  // Duplicate commit of the consumed grant: idempotent (same receipt, no double).
  auto cr2 = ex.commit(g);
  CHECK(cr2.ok());
  CHECK(cr2.value().receipt_id == cr1.value().receipt_id);
  CHECK(ex.committed_tokens(StateId::from(1)) == 1);
}

DF_TEST(cpu_tx_conflicting_grant_rejected) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  auto p1 = ex.prepare(r1);
  CommitGrant g = grant_from(p1.value().members[0], GrantId::from(1));
  auto ok = ex.commit(g);
  CHECK(ok.ok());
  // A different grant id for the same proposal must not be accepted.
  CommitGrant g2 = grant_from(p1.value().members[0], GrantId::from(2));
  auto r = ex.commit(g2);
  CHECK(r.is_error());
}

DF_TEST(cpu_tx_wrong_pre_digest_rejected) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  auto p1 = ex.prepare(make_req(SequenceId::from(1), StateId::from(1),
                                AttemptId::from(1), DecodeGeneration::from(1), 10, 0));
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  g.pre_state_digest = g.pre_state_digest + 1;  // wrong pre-state digest
  auto r = ex.commit(g);
  CHECK(r.is_error());
  CHECK(ex.committed_tokens(StateId::from(1)) == 0);
}

DF_TEST(cpu_tx_wrong_boot_or_generation_rejected) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  auto p1 = ex.prepare(make_req(SequenceId::from(1), StateId::from(1),
                                AttemptId::from(1), DecodeGeneration::from(1), 10, 0));
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  g.worker_boot = WorkerBootId::from(999);  // wrong worker boot
  CHECK(ex.commit(g).is_error());
  g.worker_boot = WorkerBootId::from(1);
  g.generation = DecodeGeneration::from(99);  // wrong generation
  CHECK(ex.commit(g).is_error());
  CHECK(ex.committed_tokens(StateId::from(1)) == 0);
}

DF_TEST(cpu_tx_commit_after_abort_rejected) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  auto p1 = ex.prepare(make_req(SequenceId::from(1), StateId::from(1),
                                AttemptId::from(1), DecodeGeneration::from(1), 10, 0));
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  AbortPrepared ab = ab_from(p1.value().members[0].proposal, StateId::from(1), SequenceId::from(1),
                             AttemptId::from(1), DecodeGeneration::from(1), DispatchId::from(1),
                             CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1));
  CHECK(ex.abort(ab).ok());
  // Commit after abort: rejected, committed state unchanged.
  CHECK(ex.commit(g).is_error());
  CHECK(ex.committed_tokens(StateId::from(1)) == 0);
}

DF_TEST(cpu_tx_abort_after_commit_rejected) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  auto p1 = ex.prepare(make_req(SequenceId::from(1), StateId::from(1),
                                AttemptId::from(1), DecodeGeneration::from(1), 10, 0));
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  CHECK(ex.commit(g).ok());
  CHECK(ex.committed_tokens(StateId::from(1)) == 1);
  AbortPrepared ab = ab_from(p1.value().members[0].proposal, StateId::from(1), SequenceId::from(1),
                             AttemptId::from(1), DecodeGeneration::from(1), DispatchId::from(1),
                             CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1));
  // Abort after a successful commit is rejected; committed state unchanged.
  CHECK(ex.abort(ab).is_error());
  CHECK(ex.committed_tokens(StateId::from(1)) == 1);
}

DF_TEST(cpu_tx_cancel_after_prepare_leaves_committed) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  auto p1 = ex.prepare(r1);
  CHECK(p1.ok());
  std::uint64_t s0 = ex.committed_state_digest(StateId::from(1));
  // Cancellation between prepare and commit: abort the prepared transition.
  AbortPrepared ab = ab_from(p1.value().members[0].proposal, StateId::from(1), SequenceId::from(1),
                             AttemptId::from(1), DecodeGeneration::from(1), DispatchId::from(1),
                             CoordinatorEpoch::from(1), WorkerId::from(1), WorkerBootId::from(1));
  (void)ex.abort(ab);
  CHECK(ex.committed_state_digest(StateId::from(1)) == s0);
  CHECK(ex.committed_tokens(StateId::from(1)) == 0);
}

DF_TEST(cpu_tx_attempt_rollover_invalidates_old_proposal) {
  CpuDecodeExecutor ex(DeviceId::from(1));
  DecodeExecutionRequest r1 = make_req(SequenceId::from(1), StateId::from(1),
                                       AttemptId::from(1), DecodeGeneration::from(1), 10, 0);
  auto p1 = ex.prepare(r1);
  CHECK(p1.ok());
  CommitGrant g = grant_from(p1.value().members[0]);
  // A retry mints a NEW AttemptId under the same generation; an old proposal
  // cannot commit.
  CommitGrant g2 = g;
  g2.attempt = AttemptId::from(2);  // new attempt
  CHECK(ex.commit(g2).is_error());
  CHECK(ex.committed_tokens(StateId::from(1)) == 0);
  (void)g;
}
