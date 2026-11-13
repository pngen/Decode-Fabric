// Regression tests for scheduling, authority, capacity, recovery and
// coordinator defects. Each test can be run alone: regression_test <name>.
#include "test_framework.hpp"
#include "process_utils.hpp"
#include "transactional_helpers.hpp"
#include "decodefabric/binary.hpp"
#include "decodefabric/clock.hpp"
#include "decodefabric/cpu_executor.hpp"
#ifdef DECODEFABRIC_CUDA_ENABLED
#include "decodefabric/cuda_executor.hpp"
#endif
#include "decodefabric/fabric.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace decodefabric;
using namespace decodefabric::protocol;
using namespace decodefabric::transport;

namespace {

CompatibilityKey key(std::uint64_t adapter = 0) {
  CompatibilityKey k;
  k.model = ModelId::from(1);
  k.revision = ModelRevision::from(1);
  k.adapter = AdapterId::from(adapter);
  k.backend = BackendKind::CPU;
  k.device = DeviceId::from(1);
  k.dtype = DType::F32;
  return k;
}

WorkerDescriptor worker(std::uint64_t id, std::uint64_t boot, const CpuDecodeExecutor& ex,
                        std::uint64_t capacity = 100, std::uint64_t adapter = 0) {
  WorkerDescriptor w;
  w.id = WorkerId::from(id);
  w.boot_id = WorkerBootId::from(boot);
  w.health = WorkerHealth::Healthy;
  w.device = ex.device();
  w.advertised_capacity = capacity;
  w.supported_models.push_back(key(adapter));
  return w;
}

DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t budget, std::uint64_t tenant = 1) {
  DecodeRequest r;
  r.id = RequestId::from(id);
  r.initial_attempt = AttemptId::from(id);
  r.tenant = TenantId::from(tenant);
  r.model = ModelId::from(1);
  r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(seq);
  r.prompt_length = 4;
  r.max_generation_length = budget;
  r.tenant_weight = 1.0;
  r.latency_class = LatencyClass::Standard;
  r.state.id = StateId::from(seq);
  r.state.estimated_growth = 64;
  return r;
}

std::unique_ptr<DecodeFabric> make_fab(FixedClock& clk, std::uint32_t max_sequences = 8,
                                       std::uint32_t max_attempts = 3) {
  DecodeFabric::Config c;
  c.clock = &clk;
  c.group_limits.max_sequences = max_sequences;
  c.retry_policy.max_attempts = max_attempts;
  return std::make_unique<DecodeFabric>(c);
}

std::vector<Dispatch> cycle(DecodeFabric& fab, FixedClock& clk) {
  clk.advance(1000);
  (void)fab.advance(clk.now());
  return fab.schedule(clk.now());
}

// Schedules and drives dispatches until nothing is active (bounded).
void run_to_idle(DecodeFabric& fab, CpuDecodeExecutor& ex, FixedClock& clk, int max_cycles = 200) {
  for (int i = 0; i < max_cycles && fab.active_sequences() > 0; ++i) {
    for (const Dispatch& d : cycle(fab, clk)) (void)df_test::drive_dispatch(fab, ex, d);
  }
}

DecodeExecutionRequest exec_request(const Dispatch& d) {
  DecodeExecutionRequest q;
  q.dispatch_id = d.id; q.epoch = d.epoch; q.worker = d.worker; q.worker_boot = d.worker_boot;
  q.key = d.key; q.device = d.device; q.reservation_id = d.reservation.value(); q.members = d.members;
  std::string k = d.key.to_string();
  q.group_payload.assign(k.begin(), k.end());
  return q;
}

DecodeExecutionResult outcome_for(const Dispatch& d, MemberOutcomeKind kind) {
  DecodeExecutionResult r;
  r.dispatch_id = d.id; r.epoch = d.epoch; r.worker = d.worker; r.worker_boot = d.worker_boot;
  MemberOutcome mo;
  mo.sequence = d.members[0].sequence;
  mo.kind = kind;
  mo.attempt = d.members[0].attempt;
  mo.generation = d.members[0].generation;
  mo.retryable = kind == MemberOutcomeKind::RetryableFailure;
  r.outcomes.push_back(mo);
  return r;
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

// Re-seals a snapshot body with a valid trailing CRC.
std::vector<std::uint8_t> seal(std::vector<std::uint8_t> body) {
  const std::uint32_t c = crc32(body.data(), body.size());
  for (int i = 0; i < 4; ++i) body.push_back(static_cast<std::uint8_t>((c >> (8 * i)) & 0xFF));
  return body;
}

std::vector<std::uint8_t> body_of(const std::vector<std::uint8_t>& snapshot) {
  return std::vector<std::uint8_t>(snapshot.begin(), snapshot.end() - 4);
}

// ---- coordinator process helpers ----------------------------------------

std::vector<std::uint8_t> hello_payload(std::uint64_t id, std::uint64_t boot) {
  binary::Writer wr;
  wr.u64(id); wr.u64(boot); wr.u8(static_cast<std::uint8_t>(WorkerHealth::Healthy));
  wr.u64(256); wr.u64(0); wr.u64(0);
  wr.u64(1);
  wr.u8(static_cast<std::uint8_t>(BackendKind::CPU));
  wr.str("regression fake worker");
  wr.u64(16ull << 30);
  wr.u32(0); wr.u32(0); wr.u32(0xFFFFFFFFu); wr.u32(0);
  wr.u64(1);
  const CompatibilityKey k = key();
  wr.u64(k.model.value()); wr.u64(k.revision.value()); wr.u64(k.adapter.value());
  wr.u8(static_cast<std::uint8_t>(k.backend)); wr.u64(k.device.value());
  wr.u8(static_cast<std::uint8_t>(k.dtype));
  wr.u32(k.tensor_layout); wr.u32(k.kv_representation); wr.u32(k.sequence_requirements); wr.u32(k.operator_policy);
  return wr.take();
}

// A refused loopback connect can take seconds on Windows, so the wait is
// bounded in time rather than in attempts.
bool coordinator_up(int port) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  while (std::chrono::steady_clock::now() < until) {
    TcpConnection c;
    if (c.connect("127.0.0.1", port).ok() && c.send_frame(FrameType::StatusQuery, {}).ok()) {
      auto fr = c.recv_frame();
      if (fr.ok() && fr.value().type == FrameType::StatusReply) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

StatusReply status_of(int port) {
  StatusReply s;
  TcpConnection c;
  if (!c.connect("127.0.0.1", port).ok()) return s;
  if (!c.send_frame(FrameType::StatusQuery, {}).ok()) return s;
  auto fr = c.recv_frame();
  if (fr.ok() && fr.value().type == FrameType::StatusReply) {
    auto d = decode_status_reply(fr.value().payload);
    if (d.ok()) return d.value();
  }
  return s;
}

bool submit_to(int port, const DecodeRequest& r) {
  for (int t = 0; t < 300; ++t) {
    TcpConnection c;
    if (c.connect("127.0.0.1", port).ok() &&
        c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)).ok()) {
      auto fr = c.recv_frame();
      if (fr.ok() && fr.value().type == FrameType::SubmitAck) {
        auto a = decode_submit_ack(fr.value().payload);
        if (a.ok() && a.value().admitted) return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::unique_ptr<TcpConnection> fake_worker(int port, std::uint64_t id, std::uint64_t boot) {
  auto c = std::make_unique<TcpConnection>();
  if (!c->connect("127.0.0.1", port).ok()) return nullptr;
  if (!c->send_frame(FrameType::Hello, hello_payload(id, boot)).ok()) return nullptr;
  (void)c->set_nonblocking(true);
  return c;
}

// Waits (bounded) for the next frame of the given type on a non-blocking connection.
bool next_frame(TcpConnection& c, FrameType type, Frame& out, int tries = 300) {
  for (int i = 0; i < tries; ++i) {
    auto f = c.recv_frame();
    if (f.ok()) {
      if (f.value().type == type) { out = f.value(); return true; }
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Re-queued work must be dispatchable again
// ---------------------------------------------------------------------------

DF_TEST(reg_u01_worker_loss_requeues_dispatchable) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  CHECK(fab->register_worker(worker(2, 20, ex)).ok());
  const SequenceId seq = SequenceId::from(101);
  CHECK(fab->submit(req(1, 101, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  CHECK(fab->mark_worker_dead(d1[0].worker).ok());
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  CHECK(fab->sequence_state(seq) == SequenceState::Dispatched);
  for (const Dispatch& d : d2) (void)df_test::drive_dispatch(*fab, ex, d);
  run_to_idle(*fab, ex, clk);
  CHECK(fab->sequence_state(seq) == SequenceState::Completed);
  CHECK(fab->sequence_generated(seq) == 3);
  CHECK(fab->active_sequences() == 0);
}

DF_TEST(reg_u02_abort_prepared_requeues_dispatchable) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(102);
  CHECK(fab->submit(req(2, 102, 2)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  auto prep = ex.prepare(exec_request(d1[0]));
  CHECK(prep.ok());
  if (!prep.ok()) return;
  auto auth = fab->authorize_prepared(prep.value());
  CHECK(auth.ok() && auth.value().members.size() == 1 && auth.value().members[0].has_grant);
  if (!auth.ok() || auth.value().members.empty()) return;
  const PreparedMember& pm = prep.value().members[0];
  AbortPrepared ab;
  ab.proposal = pm.proposal; ab.sequence = pm.sequence; ab.state = pm.state; ab.attempt = pm.attempt;
  ab.generation = pm.generation; ab.dispatch = pm.dispatch; ab.epoch = pm.epoch; ab.worker = pm.worker;
  ab.worker_boot = pm.worker_boot;
  CHECK(fab->abort_prepared(ab).ok());
  (void)ex.abort(ab);
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  CHECK(fab->sequence_state(seq) == SequenceState::Dispatched);
  for (const Dispatch& d : d2) (void)df_test::drive_dispatch(*fab, ex, d);
  run_to_idle(*fab, ex, clk);
  CHECK(fab->sequence_state(seq) == SequenceState::Completed);
}

DF_TEST(reg_u03_yielded_member_runs_again) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(103);
  CHECK(fab->submit(req(3, 103, 2)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  CHECK(fab->apply_completion(outcome_for(d1[0], MemberOutcomeKind::Yielded)).ok());
  CHECK(fab->sequence_state(seq) != SequenceState::StepCompleted);
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  for (const Dispatch& d : d2) (void)df_test::drive_dispatch(*fab, ex, d);
  run_to_idle(*fab, ex, clk);
  CHECK(fab->sequence_state(seq) == SequenceState::Completed);
  CHECK(fab->active_sequences() == 0);
}

DF_TEST(reg_u04_exhausted_retry_is_terminal) {
  FixedClock clk(1000);
  auto fab = make_fab(clk, 8, 1);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(104);
  CHECK(fab->submit(req(4, 104, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  CHECK(fab->apply_completion(outcome_for(d1[0], MemberOutcomeKind::RetryableFailure)).ok());
  CHECK(fab->sequence_state(seq) == SequenceState::NonRetryableFailure);
  CHECK(fab->active_sequences() == 0);
}

DF_TEST(reg_u05_nothing_to_generate_refused) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  DecodeRequest r = req(5, 105, 4);
  r.pregenerated = 4;
  CHECK(!fab->submit(r).admitted);
  (void)cycle(*fab, clk);
  (void)cycle(*fab, clk);
  CHECK(fab->active_sequences() == 0);
  CHECK(fab->stats().sequences_completed == 0);
}

// ---------------------------------------------------------------------------
// Capacity and reservations
// ---------------------------------------------------------------------------

DF_TEST(reg_u06_worker_capacity_returns) {
  FixedClock clk(1000);
  auto fab = make_fab(clk, 1);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex, 1)).ok());
  for (std::uint64_t i = 0; i < 3; ++i) CHECK(fab->submit(req(60 + i, 160 + i, 2)).admitted);
  run_to_idle(*fab, ex, clk, 100);
  CHECK(fab->active_sequences() == 0);
  CHECK(fab->stats().generated_tokens == 6);
}

DF_TEST(reg_u07_group_reservation_held_by_all_members) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  CHECK(fab->submit(req(71, 171, 3)).admitted);
  CHECK(fab->submit(req(72, 172, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1 && d1[0].members.size() == 2);
  if (d1.size() != 1 || d1[0].members.size() != 2) return;
  auto prep = ex.prepare(exec_request(d1[0]));
  CHECK(prep.ok());
  if (!prep.ok()) return;
  auto auth = fab->authorize_prepared(prep.value());
  CHECK(auth.ok());
  if (!auth.ok()) return;
  std::vector<MemberReceipt> receipts;
  for (const auto& ga : auth.value().members) {
    if (!ga.has_grant) continue;
    auto cr = ex.commit(ga.grant);
    if (cr.ok()) receipts.push_back(cr.value());
  }
  CHECK(receipts.size() == 2);
  if (receipts.size() != 2) return;
  ReceiptDecode first;
  first.dispatch_id = d1[0].id; first.epoch = d1[0].epoch; first.worker = d1[0].worker; first.worker_boot = d1[0].worker_boot;
  first.receipts.push_back(receipts[0]);
  CHECK(fab->apply_commit_receipt(first).ok());
  CHECK(fab->snapshot().reservations.size() == 1);   // the other member still runs
  ReceiptDecode second = first;
  second.receipts.clear();
  second.receipts.push_back(receipts[1]);
  CHECK(fab->apply_commit_receipt(second).ok());
  CHECK(fab->snapshot().reservations.empty());
}

DF_TEST(reg_u08_retry_releases_inflight) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex, 1)).ok());
  const SequenceId seq = SequenceId::from(108);
  CHECK(fab->submit(req(8, 108, 2)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  CHECK(fab->retry(seq).retried);
  CHECK(fab->snapshot().reservations.empty());
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  for (const Dispatch& d : d2) (void)df_test::drive_dispatch(*fab, ex, d);
  run_to_idle(*fab, ex, clk);
  CHECK(fab->sequence_state(seq) == SequenceState::Completed);
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

DF_TEST(reg_u09_stale_prepared_replay_keeps_live_dispatch) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(109);
  CHECK(fab->submit(req(9, 109, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  auto prep1 = ex.prepare(exec_request(d1[0]));
  CHECK(prep1.ok());
  if (!prep1.ok()) return;
  PreparedDecode old = prep1.value();
  auto auth = fab->authorize_prepared(prep1.value());
  CHECK(auth.ok());
  ReceiptDecode rd;
  rd.dispatch_id = d1[0].id; rd.epoch = d1[0].epoch; rd.worker = d1[0].worker; rd.worker_boot = d1[0].worker_boot;
  for (const auto& ga : auth.value().members) if (ga.has_grant) { auto cr = ex.commit(ga.grant); if (cr.ok()) rd.receipts.push_back(cr.value()); }
  CHECK(fab->apply_commit_receipt(rd).ok());
  CHECK(fab->sequence_generated(seq) == 1);
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  if (d2.size() != 1) return;
  (void)fab->authorize_prepared(old);   // a replay of the finished step
  auto live = fab->in_flight_authority(seq);
  CHECK(live.exists);
  CHECK(live.dispatch == d2[0].id);
  CHECK(fab->snapshot().reservations.size() == 1);
  df_test::DriveResult dr = df_test::drive_dispatch(*fab, ex, d2[0]);
  CHECK(dr.committed == 1);
  CHECK(fab->sequence_generated(seq) == 2);
}

DF_TEST(reg_u10_stale_epoch_completion_keeps_live_dispatch) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(110);
  CHECK(fab->submit(req(10, 110, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  CHECK(df_test::drive_dispatch(*fab, ex, d1[0]).committed == 1);
  CHECK(fab->roll_epoch().ok());
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  if (d2.size() != 1) return;
  (void)fab->apply_completion(outcome_for(d1[0], MemberOutcomeKind::Yielded));   // old epoch, old dispatch
  auto live = fab->in_flight_authority(seq);
  CHECK(live.exists);
  CHECK(live.dispatch == d2[0].id);
  CHECK(fab->snapshot().reservations.size() == 1);
  CHECK(df_test::drive_dispatch(*fab, ex, d2[0]).committed == 1);
  CHECK(fab->sequence_generated(seq) == 2);
}

DF_TEST(reg_u11_prepared_from_other_worker_refused) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  CHECK(fab->register_worker(worker(2, 20, ex)).ok());
  const SequenceId seq = SequenceId::from(111);
  CHECK(fab->submit(req(11, 111, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  DecodeExecutionRequest q = exec_request(d1[0]);
  const bool to_one = d1[0].worker == WorkerId::from(1);
  q.worker = WorkerId::from(to_one ? 2 : 1);
  q.worker_boot = WorkerBootId::from(to_one ? 20 : 10);
  auto prep = ex.prepare(q);
  CHECK(prep.ok());
  if (!prep.ok()) return;
  auto auth = fab->authorize_prepared(prep.value());
  CHECK(auth.ok());
  if (!auth.ok()) return;
  std::size_t grants = 0;
  for (const auto& ga : auth.value().members) if (ga.has_grant) ++grants;
  CHECK(grants == 0);
  CHECK(!fab->has_pending_grant(seq));
}

DF_TEST(reg_u12_completion_from_other_worker_refused) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  CHECK(fab->register_worker(worker(2, 20, ex)).ok());
  const SequenceId seq = SequenceId::from(112);
  CHECK(fab->submit(req(12, 112, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  DecodeExecutionResult r = outcome_for(d1[0], MemberOutcomeKind::NonRetryableFailure);
  const bool to_one = d1[0].worker == WorkerId::from(1);
  r.worker = WorkerId::from(to_one ? 2 : 1);
  r.worker_boot = WorkerBootId::from(to_one ? 20 : 10);
  (void)fab->apply_completion(r);
  CHECK(fab->sequence_state(seq) == SequenceState::Dispatched);
  CHECK(fab->active_sequences() == 1);
}

DF_TEST(reg_u13_completion_for_undispatched_refused) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(113);
  CHECK(fab->submit(req(13, 113, 3)).admitted);
  DecodeExecutionResult r;
  r.epoch = fab->epoch();
  r.worker = WorkerId::from(77);
  MemberOutcome mo;
  mo.sequence = seq;
  mo.kind = MemberOutcomeKind::Cancelled;
  mo.attempt = AttemptId::from(13);
  mo.generation = DecodeGeneration::from(0);
  r.outcomes.push_back(mo);
  (void)fab->apply_completion(r);   // nothing was ever dispatched for it
  CHECK(fab->sequence_state(seq) == SequenceState::Ready);
  CHECK(fab->active_sequences() == 1);
  CHECK(fab->stats().sequences_cancelled == 0);
}

DF_TEST(reg_u14_restarted_worker_requeues) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(114);
  CHECK(fab->submit(req(14, 114, 2)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  CHECK(fab->register_worker(worker(1, 11, ex)).ok());   // the worker process restarted
  auto d2 = cycle(*fab, clk);
  CHECK(d2.size() == 1);
  if (d2.size() != 1) return;
  CHECK(d2[0].worker_boot == WorkerBootId::from(11));
  (void)df_test::drive_dispatch(*fab, ex, d2[0]);
  run_to_idle(*fab, ex, clk);
  CHECK(fab->sequence_state(seq) == SequenceState::Completed);
}

DF_TEST(reg_u15_duplicate_prepared_keeps_grant) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(115);
  CHECK(fab->submit(req(15, 115, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  auto prep = ex.prepare(exec_request(d1[0]));
  CHECK(prep.ok());
  if (!prep.ok()) return;
  auto auth = fab->authorize_prepared(prep.value());
  CHECK(auth.ok());
  if (!auth.ok()) return;
  (void)fab->authorize_prepared(prep.value());   // the same prepared result again
  ReceiptDecode rd;
  rd.dispatch_id = d1[0].id; rd.epoch = d1[0].epoch; rd.worker = d1[0].worker; rd.worker_boot = d1[0].worker_boot;
  for (const auto& ga : auth.value().members) if (ga.has_grant) { auto cr = ex.commit(ga.grant); if (cr.ok()) rd.receipts.push_back(cr.value()); }
  CHECK(rd.receipts.size() == 1);
  CHECK(fab->apply_commit_receipt(rd).ok());
  CHECK(fab->sequence_generated(seq) == 1);
  CHECK(ex.committed_tokens(StateId::from(115)) == 1);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

DF_TEST(reg_u16_recovery_requeues_inflight) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(116);
  CHECK(fab->submit(req(16, 116, 2)).admitted);
  CHECK(cycle(*fab, clk).size() == 1);
  auto bytes = fab->serialize_state();
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  FixedClock clk2(5000);
  auto fab2 = make_fab(clk2);
  CHECK(fab2->recover_state(bytes.value()).ok());
  CHECK(fab2->register_worker(worker(1, 12, ex)).ok());
  auto d = cycle(*fab2, clk2);
  CHECK(d.size() == 1);
  for (const Dispatch& x : d) (void)df_test::drive_dispatch(*fab2, ex, x);
  CpuDecodeExecutor fresh(DeviceId::from(1));
  run_to_idle(*fab2, fresh, clk2);
  CHECK(fab2->sequence_state(seq) == SequenceState::Completed);
}

DF_TEST(reg_u17_recovery_refuses_bad_state_and_keeps_live) {
  FixedClock clk(1000);
  auto src = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(src->register_worker(worker(1, 10, ex)).ok());
  CHECK(src->submit(req(17, 117, 3)).admitted);
  auto good = src->serialize_state();
  CHECK(good.ok());
  if (!good.ok()) return;

  FixedClock clk2(1000);
  auto live = make_fab(clk2);
  CHECK(live->register_worker(worker(1, 10, ex)).ok());
  CHECK(live->submit(req(18, 118, 3)).admitted);

  std::vector<std::uint8_t> trailing = body_of(good.value());
  for (int i = 0; i < 4; ++i) trailing.push_back(0);
  CHECK(live->recover_state(seal(trailing)).is_error());

  std::vector<std::uint8_t> bad_state = body_of(good.value());
  CHECK(bad_state.size() > 157);
  if (bad_state.size() <= 157) return;
  bad_state[157] = 77;   // the first sequence record's state byte
  CHECK(live->recover_state(seal(bad_state)).is_error());

  std::vector<std::uint8_t> truncated = body_of(good.value());
  truncated.resize(truncated.size() - 10);
  CHECK(live->recover_state(seal(truncated)).is_error());

  // The refused snapshots left the live fabric as it was.
  CHECK(live->active_sequences() == 1);
  CHECK(live->sequence_state(SequenceId::from(118)) == SequenceState::Ready);
  CHECK(live->sequence_state(SequenceId::from(117)) == SequenceState::Admitted);   // unknown here

  // An empty version-3 snapshot is still read.
  binary::Writer w;
  w.u8('D'); w.u8('F'); w.u8('S'); w.u8('T'); w.u32(3);
  w.u64(4); w.u64(1); w.u64(1); w.u64(0); w.u64(0); w.u64(0); w.u64(0);
  CHECK(live->recover_state(seal(w.take())).ok());
  CHECK(live->epoch() == CoordinatorEpoch::from(4));
}

DF_TEST(reg_u18_recovery_counts_only_live_tenant_work) {
  FixedClock clk(1000);
  DecodeFabric::Config c;
  c.clock = &clk;
  c.group_limits.max_tenant_share = 1;
  DecodeFabric fab(c);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab.register_worker(worker(1, 10, ex)).ok());
  CHECK(fab.submit(req(19, 119, 1, 5)).admitted);
  run_to_idle(fab, ex, clk);
  CHECK(fab.sequence_state(SequenceId::from(119)) == SequenceState::Completed);
  auto bytes = fab.serialize_state();
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  FixedClock clk2(1000);
  DecodeFabric::Config c2 = c;
  c2.clock = &clk2;
  DecodeFabric fab2(c2);
  CHECK(fab2.recover_state(bytes.value()).ok());
  CHECK(fab2.register_worker(worker(1, 10, ex)).ok());
  CHECK(fab2.submit(req(20, 120, 1, 5)).admitted);   // the finished sequence holds no share
}

DF_TEST(reg_u19_recovery_keeps_request_identity) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex, 100, 7)).ok());
  DecodeRequest r = req(21, 121, 3);
  r.adapter = AdapterId::from(7);
  r.device_constraint = DeviceId::from(1);
  r.sampling_metadata = std::string("\x01\x02", 2);
  CHECK(fab->submit(r).admitted);
  auto bytes = fab->serialize_state();
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  FixedClock clk2(1000);
  auto fab2 = make_fab(clk2);
  CHECK(fab2->recover_state(bytes.value()).ok());
  CHECK(fab2->register_worker(worker(1, 10, ex, 100, 7)).ok());
  auto d = cycle(*fab2, clk2);
  CHECK(d.size() == 1);
  if (d.size() != 1 || d[0].members.empty()) return;
  CHECK(d[0].key.adapter == AdapterId::from(7));
  CHECK(d[0].members[0].payload == std::vector<std::uint8_t>({0x01, 0x02}));
}

DF_TEST(reg_u20_recovery_never_reuses_grant_ids) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex)).ok());
  const SequenceId seq = SequenceId::from(122);
  CHECK(fab->submit(req(22, 122, 3)).admitted);
  auto d1 = cycle(*fab, clk);
  CHECK(d1.size() == 1);
  if (d1.size() != 1) return;
  CHECK(df_test::drive_dispatch(*fab, ex, d1[0]).committed == 1);
  auto bytes = fab->serialize_state();
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  FixedClock clk2(1000);
  auto fab2 = make_fab(clk2);
  CHECK(fab2->recover_state(bytes.value()).ok());
  CHECK(fab2->register_worker(worker(1, 10, ex)).ok());
  auto d2 = cycle(*fab2, clk2);
  CHECK(d2.size() == 1);
  if (d2.size() != 1) return;
  auto prep = ex.prepare(exec_request(d2[0]));
  CHECK(prep.ok());
  if (!prep.ok()) return;
  auto auth = fab2->authorize_prepared(prep.value());
  CHECK(auth.ok());
  if (!auth.ok()) return;
  std::size_t grants = 0;
  for (const auto& ga : auth.value().members) {
    if (!ga.has_grant) continue;
    ++grants;
    CHECK(ga.grant.grant_id.value() > 1);
    CHECK(fab2->grant_status(ga.grant.grant_id) == 0);
  }
  CHECK(grants == 1);
  CHECK(fab2->has_pending_grant(seq));
}

DF_TEST(reg_u21_recovery_returns_live_reservations) {
  FixedClock clk(1000);
  auto fab = make_fab(clk);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab->register_worker(worker(1, 10, ex, 1)).ok());
  CHECK(fab->submit(req(23, 123, 3)).admitted);
  CHECK(cycle(*fab, clk).size() == 1);
  CHECK(fab->snapshot().reservations.size() == 1);
  FixedClock clk0(1000);
  auto empty = make_fab(clk0);
  auto bytes = empty->serialize_state();
  CHECK(bytes.ok());
  if (!bytes.ok()) return;
  CHECK(fab->recover_state(bytes.value()).ok());
  CHECK(fab->snapshot().reservations.empty());
  CHECK(fab->submit(req(24, 124, 1)).admitted);
  CHECK(cycle(*fab, clk).size() == 1);   // the worker's only unit is free again
}

DF_TEST(reg_u22_zero_event_capacity) {
  FixedClock clk(1000);
  DecodeFabric::Config c;
  c.clock = &clk;
  c.event_capacity = 0;
  DecodeFabric fab(c);
  CpuDecodeExecutor ex(DeviceId::from(1));
  CHECK(fab.register_worker(worker(1, 10, ex)).ok());
  CHECK(fab.submit(req(25, 125, 2)).admitted);
  run_to_idle(fab, ex, clk);
  CHECK(fab.active_sequences() == 0);
  CHECK(fab.events().empty());
}

#ifdef DECODEFABRIC_CUDA_ENABLED
// A CUDA worker that adopts a sequence (another worker ran it, or it
// restarted) rebuilds the committed state from the committed-token count. The
// rebuilt state must be the one the device steps produced, or no later step
// can be authorized against the recorded committed-state digest.
DF_TEST(reg_u23_cuda_rebuilt_state_matches_device_steps) {
  DecodeMemberSpec m;
  m.sequence = SequenceId::from(201);
  m.attempt = AttemptId::from(1);
  m.state.id = StateId::from(201);
  m.remaining_budget = 16;
  CudaDecodeExecutor stepped(DeviceId::from(1), 0);
  for (std::uint64_t step = 0; step < 3; ++step) {
    DecodeExecutionRequest q;
    q.dispatch_id = DispatchId::from(step + 1); q.epoch = CoordinatorEpoch::from(1);
    q.worker = WorkerId::from(1); q.worker_boot = WorkerBootId::from(1);
    m.generated_tokens = step;
    m.generation = DecodeGeneration::from(step + 1);
    q.members = {m};
    auto prep = stepped.prepare(q);
    CHECK(prep.ok());
    if (!prep.ok() || prep.value().members.empty()) return;
    const PreparedMember& pm = prep.value().members[0];
    CommitGrant g;
    g.grant_id = GrantId::from(step + 1); g.proposal = pm.proposal; g.epoch = q.epoch;
    g.worker = q.worker; g.worker_boot = q.worker_boot; g.sequence = pm.sequence; g.state = pm.state;
    g.attempt = pm.attempt; g.generation = pm.generation; g.dispatch = q.dispatch_id;
    g.committed_position = pm.committed_position_before;
    g.pre_state_digest = pm.pre_state_digest; g.post_state_digest = pm.post_state_digest;
    CHECK(stepped.commit(g).ok());
  }
  CudaDecodeExecutor adopting(DeviceId::from(1), 0);
  DecodeExecutionRequest q;
  q.dispatch_id = DispatchId::from(10); q.epoch = CoordinatorEpoch::from(1);
  q.worker = WorkerId::from(2); q.worker_boot = WorkerBootId::from(2);
  m.generated_tokens = 3;
  m.generation = DecodeGeneration::from(4);
  q.members = {m};
  auto prep = adopting.prepare(q);
  CHECK(prep.ok());
  if (!prep.ok() || prep.value().members.empty()) return;
  CHECK(prep.value().members[0].pre_state_digest == stepped.committed_state_digest(StateId::from(201)));
}
#endif

// ---------------------------------------------------------------------------
// Coordinator process
// ---------------------------------------------------------------------------

DF_TEST(reg_c01_replaced_connection_keeps_restarted_worker) {
  const int port = 48311;
  SpawnedProcess coord = spawn_process(tools_dir() + "/df-coordinator.exe", std::to_string(port), ".");
  CHECK(coord.ok);
  if (!coord.ok) return;
  CHECK(coordinator_up(port));
  auto old_conn = fake_worker(port, 1, 100);
  CHECK(old_conn != nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto new_conn = fake_worker(port, 1, 200);   // the worker restarted
  CHECK(new_conn != nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  old_conn.reset();   // the previous process's connection ends afterwards
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(submit_to(port, req(31, 131, 2)));
  Frame f;
  CHECK(new_conn != nullptr && next_frame(*new_conn, FrameType::ExecuteRequest, f));
  coord.kill();
}

DF_TEST(reg_c02_prepared_for_another_worker_ignored) {
  const int port = 48312;
  SpawnedProcess coord = spawn_process(tools_dir() + "/df-coordinator.exe", std::to_string(port), ".");
  CHECK(coord.ok);
  if (!coord.ok) return;
  CHECK(coordinator_up(port));
  auto a = fake_worker(port, 1, 100);
  auto b = fake_worker(port, 2, 200);
  CHECK(a != nullptr && b != nullptr);
  if (!a || !b) { coord.kill(); return; }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(submit_to(port, req(32, 132, 2)));
  TcpConnection* got = nullptr;
  TcpConnection* other = nullptr;
  Frame f;
  for (int i = 0; i < 300 && !got; ++i) {
    if (next_frame(*a, FrameType::ExecuteRequest, f, 1)) { got = a.get(); other = b.get(); }
    else if (next_frame(*b, FrameType::ExecuteRequest, f, 1)) { got = b.get(); other = a.get(); }
    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(got != nullptr);
  if (!got) { coord.kill(); return; }
  auto dreq = decode_execute_request(f.payload);
  CHECK(dreq.ok());
  if (!dreq.ok() || dreq.value().members.empty()) { coord.kill(); return; }
  const DecodeExecutionRequest& q = dreq.value();
  PreparedDecode pd;
  pd.dispatch_id = q.dispatch_id; pd.epoch = q.epoch; pd.worker = q.worker; pd.worker_boot = q.worker_boot;
  PreparedMember pm;
  pm.sequence = q.members[0].sequence; pm.state = q.members[0].state.id; pm.attempt = q.members[0].attempt;
  pm.generation = q.members[0].generation; pm.dispatch = q.dispatch_id; pm.epoch = q.epoch;
  pm.worker = q.worker; pm.worker_boot = q.worker_boot; pm.proposal = ProposalId::from(9001);
  pm.pre_state_digest = 1; pm.post_state_digest = 2; pm.delta_digest = 3;
  pm.committed_position_before = 0; pm.committed_position_after = 1;
  pm.outcome.sequence = pm.sequence; pm.outcome.kind = MemberOutcomeKind::StepSuccessContinue;
  pm.outcome.attempt = pm.attempt; pm.outcome.generation = pm.generation; pm.outcome.generated = 1;
  pd.members.push_back(pm);
  // The worker that was not dispatched to claims the other worker's step.
  CHECK(other->send_frame(FrameType::PreparedResult, encode_prepared_result(pd)).ok());
  Frame g;
  CHECK(!next_frame(*other, FrameType::CommitGrant, g, 100));
  coord.kill();
}

DF_TEST(reg_c03_coordinator_expires_deadlines) {
  const int port = 48313;
  SpawnedProcess coord = spawn_process(tools_dir() + "/df-coordinator.exe", std::to_string(port), ".");
  CHECK(coord.ok);
  if (!coord.ok) return;
  CHECK(coordinator_up(port));
  auto w = fake_worker(port, 1, 100);   // never answers
  CHECK(w != nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  DecodeRequest r = req(33, 133, 4);
  r.deadline = TimePoint(1);   // long past on the coordinator's monotonic clock
  CHECK(submit_to(port, r));
  bool expired = false;
  for (int i = 0; i < 300 && !expired; ++i) {
    StatusReply s = status_of(port);
    if (s.requests_admitted == 1 && s.active == 0) expired = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(expired);
  coord.kill();
}

DF_TEST(reg_c04_loop_count_serves_then_exits) {
  const int port = 48314;
  SpawnedProcess coord = spawn_process(tools_dir() + "/df-coordinator.exe", std::to_string(port) + " 600", ".");
  CHECK(coord.ok);
  if (!coord.ok) return;
  CHECK(coordinator_up(port));
  const DWORD rc = WaitForSingleObject(coord.h, 30000);
  CHECK(rc == WAIT_OBJECT_0);
  coord.kill();
}
