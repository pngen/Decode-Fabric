#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/binary.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace decodefabric {

using namespace decodefabric::protocol;
using namespace decodefabric::transport;
using decodefabric::binary::Reader;
using decodefabric::binary::Writer;

namespace {

// ---------------------------------------------------------------------------
// Worker descriptor (de)serialization over the wire.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> enc_worker(const WorkerDescriptor& w) {
  Writer wr;
  wr.u64(w.id.value()); wr.u64(w.boot_id.value());
  wr.u8(static_cast<std::uint8_t>(w.health));
  wr.u64(w.advertised_capacity); wr.u64(w.active_reservations); wr.u64(w.generation);
  wr.u64(w.device.id.value());
  wr.u8(static_cast<std::uint8_t>(w.device.backend));
  wr.str(w.device.name);
  wr.u64(w.device.memory_bytes);
  wr.u32(w.device.compute_capability_major); wr.u32(w.device.compute_capability_minor);
  wr.u32(w.device.supported_dtypes); wr.u32(w.device.max_groups_concurrent);
  wr.u64(static_cast<std::uint64_t>(w.supported_models.size()));
  for (const CompatibilityKey& k : w.supported_models) {
    wr.u64(k.model.value()); wr.u64(k.revision.value()); wr.u64(k.adapter.value());
    wr.u8(static_cast<std::uint8_t>(k.backend));
    wr.u64(k.device.value());
    wr.u8(static_cast<std::uint8_t>(k.dtype));
    wr.u32(k.tensor_layout); wr.u32(k.kv_representation);
    wr.u32(k.sequence_requirements); wr.u32(k.operator_policy);
  }
  return wr.take();
}

Result<WorkerDescriptor> dec_worker(const std::vector<std::uint8_t>& bytes) {
  Reader rd(bytes);
  WorkerDescriptor w;
  auto a = rd.u64(); if (!a.ok()) return Result<WorkerDescriptor>{a.error()};
  auto b = rd.u64(); if (!b.ok()) return Result<WorkerDescriptor>{b.error()};
  auto c = rd.u8(); if (!c.ok()) return Result<WorkerDescriptor>{c.error()};
  auto d = rd.u64(); if (!d.ok()) return Result<WorkerDescriptor>{d.error()};
  auto e = rd.u64(); if (!e.ok()) return Result<WorkerDescriptor>{e.error()};
  auto f = rd.u64(); if (!f.ok()) return Result<WorkerDescriptor>{f.error()};
  auto g = rd.u64(); if (!g.ok()) return Result<WorkerDescriptor>{g.error()};
  auto h = rd.u8(); if (!h.ok()) return Result<WorkerDescriptor>{h.error()};
  auto i = rd.str(); if (!i.ok()) return Result<WorkerDescriptor>{i.error()};
  auto j = rd.u64(); if (!j.ok()) return Result<WorkerDescriptor>{j.error()};
  auto m1 = rd.u32(); if (!m1.ok()) return Result<WorkerDescriptor>{m1.error()};
  auto m2 = rd.u32(); if (!m2.ok()) return Result<WorkerDescriptor>{m2.error()};
  auto m3 = rd.u32(); if (!m3.ok()) return Result<WorkerDescriptor>{m3.error()};
  auto m4 = rd.u32(); if (!m4.ok()) return Result<WorkerDescriptor>{m4.error()};
  auto n = rd.u64(); if (!n.ok()) return Result<WorkerDescriptor>{n.error()};
  if (n.value() > 4096) return Result<WorkerDescriptor>{Error{ErrorCode::ProtocolInvalidField, "too many models"}};
  for (std::uint64_t k = 0; k < n.value(); ++k) {
    CompatibilityKey key;
    auto ka = rd.u64(); if (!ka.ok()) return Result<WorkerDescriptor>{ka.error()};
    auto kb = rd.u64(); if (!kb.ok()) return Result<WorkerDescriptor>{kb.error()};
    auto kc = rd.u64(); if (!kc.ok()) return Result<WorkerDescriptor>{kc.error()};
    auto kd = rd.u8(); if (!kd.ok()) return Result<WorkerDescriptor>{kd.error()};
    auto ke = rd.u64(); if (!ke.ok()) return Result<WorkerDescriptor>{ke.error()};
    auto kf = rd.u8(); if (!kf.ok()) return Result<WorkerDescriptor>{kf.error()};
    auto kg = rd.u32(); if (!kg.ok()) return Result<WorkerDescriptor>{kg.error()};
    auto kh = rd.u32(); if (!kh.ok()) return Result<WorkerDescriptor>{kh.error()};
    auto ki = rd.u32(); if (!ki.ok()) return Result<WorkerDescriptor>{ki.error()};
    auto kj = rd.u32(); if (!kj.ok()) return Result<WorkerDescriptor>{kj.error()};
    key.model = ModelId::from(ka.value()); key.revision = ModelRevision::from(kb.value());
    key.adapter = AdapterId::from(kc.value()); key.backend = static_cast<BackendKind>(kd.value());
    key.device = DeviceId::from(ke.value()); key.dtype = static_cast<DType>(kf.value());
    key.tensor_layout = kg.value(); key.kv_representation = kh.value();
    key.sequence_requirements = ki.value(); key.operator_policy = kj.value();
    w.supported_models.push_back(key);
  }
  w.id = WorkerId::from(a.value()); w.boot_id = WorkerBootId::from(b.value());
  w.health = static_cast<WorkerHealth>(c.value());
  w.advertised_capacity = d.value(); w.active_reservations = e.value(); w.generation = f.value();
  w.device.id = DeviceId::from(g.value()); w.device.backend = static_cast<BackendKind>(h.value());
  w.device.name = i.value(); w.device.memory_bytes = j.value();
  w.device.compute_capability_major = m1.value(); w.device.compute_capability_minor = m2.value();
  w.device.supported_dtypes = m3.value(); w.device.max_groups_concurrent = m4.value();
  return Result<WorkerDescriptor>::ok(w);
}

CompatibilityKey make_supported_key(DeviceId device, BackendKind backend) {
  CompatibilityKey k;
  k.model = ModelId::from(1);
  k.revision = ModelRevision::from(1);
  k.backend = backend;
  k.device = device;
  k.dtype = DType::F32;
  return k;
}

// Global worker connection registry (owned by the coordinator).
std::mutex g_worker_mu;
std::map<WorkerId, std::shared_ptr<TcpConnection>> g_worker_conns;

}  // namespace

// ===========================================================================
// Coordinator
// ===========================================================================
Result<void> run_coordinator(DecodeFabric& fab, int listen_port, int max_loops, bool& running) {
  TcpListener listener;
  auto lr = listener.listen("127.0.0.1", listen_port);
  if (!lr.ok()) return failed<void>(ErrorCode::BackendError, "coordinator bind failed: " + lr.error().message);

  // Accept thread.
  std::thread accept_thread([&fab, &listener, &running]() {
    while (running) {
      auto conn = listener.accept();
      if (!conn.ok()) {
        if (!running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      std::shared_ptr<TcpConnection> sp = std::make_shared<TcpConnection>(std::move(conn.value()));
      // Handler thread.
      std::thread([&fab, sp, &running]() {
        // First frame should be a Hello (worker) or a client request.
        auto first = sp->recv_frame();
        if (!first.ok()) return;
        Frame fr = first.value();
        if (fr.type == FrameType::Hello) {
          auto wd = dec_worker(fr.payload);
          if (!wd.ok()) return;
          WorkerDescriptor w = wd.value();
          (void)fab.register_worker(w);
          {
            std::lock_guard<std::mutex> lk(g_worker_mu);
            g_worker_conns[w.id] = sp;
          }
          // Worker I/O loop: read ExecuteResults, detect death.
          while (running) {
            auto next = sp->recv_frame();
            if (!next.ok()) {
              (void)fab.mark_worker_dead(w.id);
              std::lock_guard<std::mutex> lk(g_worker_mu);
              g_worker_conns.erase(w.id);
              break;
            }
            Frame f = next.value();
            if (f.type == FrameType::ExecuteResult) {
              auto res = decode_execute_result(f.payload);
              if (res.ok()) (void)fab.apply_completion(res.value());
            } else if (f.type == FrameType::Shutdown) {
              std::lock_guard<std::mutex> lk(g_worker_mu);
              g_worker_conns.erase(w.id);
              break;
            }
          }
        } else {
          // Client-style connection (submission, cancellation, status, snapshot,
          // and stale ExecuteResults for the closure proof).
          Frame f = fr;  // process the frame we already read
          while (running) {
            switch (f.type) {
              case FrameType::SubmitRequest: {
                auto req = decode_submit_request(f.payload);
                if (!req.ok()) break;
                AdmissionDecision d = fab.submit(req.value());
                SubmitAck a; a.admitted = d.admitted; a.sequence = d.sequence; a.reason = d.reason;
                (void)sp->send_frame(FrameType::SubmitAck, encode_submit_ack(a));
                break;
              }
              case FrameType::CancelRequest: {
                auto q = decode_query(f.payload);
                if (q.ok()) { CancelResult r = fab.cancel_request(q.value().request); (void)sp->send_frame(FrameType::Acknowledge, std::vector<std::uint8_t>{static_cast<std::uint8_t>(r.cancelled ? 1 : 0)}); }
                break;
              }
              case FrameType::StatusQuery: {
                Snapshot sn = fab.snapshot();
                StatusReply s; s.active = static_cast<std::uint32_t>(sn.active); s.ready = static_cast<std::uint32_t>(sn.ready);
                s.generated_tokens = sn.stats.generated_tokens; s.decode_steps = sn.stats.decode_steps;
                s.groups = static_cast<std::uint32_t>(sn.groups.size()); s.requests_admitted = sn.stats.requests_admitted;
                s.stale_rejections = fab.stale_rejections(); s.epoch = fab.epoch().value();
                (void)sp->send_frame(FrameType::StatusReply, encode_status_reply(s));
                break;
              }
              case FrameType::SnapshotRequest: {
                Snapshot sn = fab.snapshot();
                (void)sp->send_frame(FrameType::SnapshotReply, encode_snapshot_reply(sn.to_json()));
                break;
              }
              case FrameType::GetAuthority: {
                auto q = decode_get_authority(f.payload);
                AuthorityReply a;
                if (q.ok()) {
                  a.sequence = q.value().sequence;
                  auto ia = fab.in_flight_authority(q.value().sequence);
                  if (ia.exists) {
                    a.exists = true; a.dispatch = ia.dispatch; a.epoch = ia.epoch;
                    a.worker = ia.worker; a.worker_boot = ia.worker_boot;
                    a.attempt = ia.attempt; a.generation = ia.generation; a.generated = ia.generated;
                  }
                }
                (void)sp->send_frame(FrameType::AuthorityReply, encode_authority_reply(a));
                break;
              }
              case FrameType::ExecuteResult: {
                // Stale replay path (closure proof / adversarial tests).
                auto res = decode_execute_result(f.payload);
                if (res.ok()) {
                  (void)fab.apply_completion(res.value());
                  Stats sn2 = fab.stats();
                  StatusReply s; s.active = fab.active_sequences(); s.ready = fab.ready_sequences();
                  s.generated_tokens = sn2.generated_tokens; s.decode_steps = sn2.decode_steps;
                  s.groups = 0; s.requests_admitted = sn2.requests_admitted;
                  (void)sp->send_frame(FrameType::StatusReply, encode_status_reply(s));
                }
                break;
              }
              case FrameType::ExplainRequest: {
                auto eq = decode_explain_request(f.payload);
                ExplainReply erp;
                if (eq.ok()) {
                  Explain ex = fab.explain(eq.value().sequence, eq.value().question);
                  erp.text = ex.answer; erp.json = ex.to_json();
                } else { erp.text = "bad explain request"; erp.json = "{}"; }
                (void)sp->send_frame(FrameType::ExplainReply, encode_explain_reply(erp));
                break;
              }
              case FrameType::RollEpoch: {
                (void)fab.roll_epoch();
                Snapshot sn3 = fab.snapshot();
                StatusReply s3; s3.active = static_cast<std::uint32_t>(sn3.active); s3.ready = static_cast<std::uint32_t>(sn3.ready);
                s3.generated_tokens = sn3.stats.generated_tokens; s3.decode_steps = sn3.stats.decode_steps; s3.groups = 0;
                s3.requests_admitted = sn3.stats.requests_admitted; s3.stale_rejections = fab.stale_rejections(); s3.epoch = fab.epoch().value();
                (void)sp->send_frame(FrameType::StatusReply, encode_status_reply(s3));
                break;
              }
              case FrameType::Shutdown:
                return;
              default: break;
            }
            auto next = sp->recv_frame();
            if (!next.ok()) break;
            f = next.value();
          }
        }
      }).detach();
    }
  });

  // Schedule/dispatch thread.
  std::thread schedule_thread([&fab, &running]() {
    while (running) {
      auto ds = fab.schedule(TimePoint(0));
      for (const Dispatch& d : ds) {
        std::shared_ptr<TcpConnection> conn;
        { std::lock_guard<std::mutex> lk(g_worker_mu); auto it = g_worker_conns.find(d.worker); if (it != g_worker_conns.end()) conn = it->second; }
        if (!conn) continue;
        DecodeExecutionRequest req;
        req.dispatch_id = d.id; req.epoch = d.epoch; req.worker = d.worker; req.worker_boot = d.worker_boot;
        req.key = d.key; req.device = d.device; req.reservation_id = d.reservation.value(); req.members = d.members;
        { std::string k = d.key.to_string(); req.group_payload.assign(k.begin(), k.end()); }
        (void)conn->send_frame(FrameType::ExecuteRequest, encode_execute_request(req));
      }
      if (running) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  // Run until stopped or max_loops.
  long loops = 0;
  while (running) {
    if (max_loops > 0 && loops >= max_loops) { running = false; break; }
    ++loops;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  running = false;
  if (accept_thread.joinable()) accept_thread.join();
  if (schedule_thread.joinable()) schedule_thread.join();
  return Result<void>::success();
}

int coordinator_main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: df-coordinator <port> [loops]\n"); return 2; }
  int port = std::atoi(argv[1]);
  int loops = argc >= 3 ? std::atoi(argv[2]) : -1;
  bool running = loops < 0;  // if loops provided, run that many then stop
  DecodeFabric::Config cfg;
  cfg.group_limits.max_sequences = 64;
  DecodeFabric fab(cfg);
  auto r = run_coordinator(fab, port, loops, running);
  if (!r.ok()) { std::fprintf(stderr, "coordinator error: %s\n", r.error().message.c_str()); return 1; }
  return 0;
}

// ===========================================================================
// Worker
// ===========================================================================
int worker_main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: df-worker <host> <port> <worker_id> <boot_id> <device_id>\n"); return 2; }
  std::string host = argv[1];
  int port = std::atoi(argv[2]);
  WorkerId wid = WorkerId::from(std::strtoull(argv[3], nullptr, 10));
  WorkerBootId boot = WorkerBootId::from(std::strtoull(argv[4], nullptr, 10));
  DeviceId dev = DeviceId::from(std::strtoull(argv[5], nullptr, 10));

  TcpConnection conn;
  // Retry connect: the coordinator may still be binding when the worker is
  // spawned. This is startup liveness, not a validation timeout.
  auto cr = conn.connect(host, port);
  for (int tries = 0; !cr.ok() && tries < 500; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cr = conn.connect(host, port);
  }
  if (!cr.ok()) { std::fprintf(stderr, "worker connect failed: %s\n", cr.error().message.c_str()); return 1; }

  CpuDecodeExecutor ex(dev);
  WorkerDescriptor w;
  w.id = wid; w.boot_id = boot; w.health = WorkerHealth::Healthy;
  w.device = ex.device(); w.advertised_capacity = 256;
  w.supported_models.push_back(make_supported_key(dev, BackendKind::CPU));
  (void)conn.send_frame(FrameType::Hello, enc_worker(w));

  while (true) {
    auto f = conn.recv_frame();
    if (!f.ok()) break;
    Frame fr = f.value();
    if (fr.type == FrameType::ExecuteRequest) {
      auto req = decode_execute_request(fr.payload);
      if (!req.ok()) { continue; }
      auto res = ex.execute(req.value());
      if (res.ok()) {
        (void)conn.send_frame(FrameType::ExecuteResult, encode_execute_result(res.value()));
      } else {
        DecodeExecutionResult r;
        r.dispatch_id = req.value().dispatch_id; r.epoch = req.value().epoch; r.worker = req.value().worker; r.worker_boot = req.value().worker_boot;
        r.group_error = res.error().code; r.group_error_message = res.error().message;
        (void)conn.send_frame(FrameType::ExecuteResult, encode_execute_result(r));
      }
    } else if (fr.type == FrameType::Shutdown) {
      (void)conn.send_frame(FrameType::WorkerShutdownAck, {});
      break;
    }
  }
  return 0;
}

// ===========================================================================
// Client driver helper
// ===========================================================================
Result<SubmitAck> client_submit(const std::string& host, int port, const DecodeRequest& req) {
  TcpConnection c;
  auto cr = c.connect(host, port);
  if (!cr.ok()) return Result<SubmitAck>{cr.error()};
  auto sr = c.send_frame(FrameType::SubmitRequest, encode_submit_request(req));
  if (!sr.ok()) return Result<SubmitAck>{sr.error()};
  auto fr = c.recv_frame();
  if (!fr.ok()) return Result<SubmitAck>{fr.error()};
  if (fr.value().type != FrameType::SubmitAck) return Result<SubmitAck>{Error{ErrorCode::ProtocolMalformed, "expected SubmitAck"}};
  return decode_submit_ack(fr.value().payload);
}

Result<StatusReply> client_status(const std::string& host, int port) {
  TcpConnection c;
  auto cr = c.connect(host, port);
  if (!cr.ok()) return Result<StatusReply>{cr.error()};
  (void)c.send_frame(FrameType::StatusQuery, {});
  auto fr = c.recv_frame();
  if (!fr.ok()) return Result<StatusReply>{fr.error()};
  if (fr.value().type != FrameType::StatusReply) return Result<StatusReply>{Error{ErrorCode::ProtocolMalformed, "expected StatusReply"}};
  return decode_status_reply(fr.value().payload);
}

}  // namespace decodefabric
