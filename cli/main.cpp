#include "decodefabric/fabric.hpp"
#include "decodefabric/cpu_executor.hpp"
#include "decodefabric/clock.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include "decodefabric/persistence.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace decodefabric {
int coordinator_main(int argc, char** argv);
int worker_main(int argc, char** argv);
}
using namespace decodefabric;
using namespace decodefabric::protocol;
using namespace decodefabric::transport;

static void usage() {
  std::fprintf(stderr,
    "DecodeFabric commands:\n"
    "  serve <port> [loops]\n"
    "  worker <host> <port> <worker_id> <boot_id> <device_id>\n"
    "  submit <host> <port> <seq> <tenant> <budget> [--model N] [--rev N]\n"
    "  status <host> <port>\n"
    "  stats <host> <port>\n"
    "  snapshot <host> <port>\n"
    "  cancel <host> <port> <request_id>\n"
    "  explain <host> <port> <seq> <question>\n"
    "  inspect <host> <port> <seq>\n"
    "  bench [sequences] [budget]\n"
    "  recover <dir>\n");
}


static int cmd_submit(int argc, char** argv) {
  (void)argc;
  std::string host = argv[2]; int port = std::atoi(argv[3]);
  std::uint64_t seq = std::strtoull(argv[4], nullptr, 10);
  std::uint64_t tenant = std::strtoull(argv[5], nullptr, 10);
  std::uint64_t budget = std::strtoull(argv[6], nullptr, 10);
  std::uint64_t model = 1, rev = 1;
  for (int i = 7; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--model")) model = std::strtoull(argv[i+1], nullptr, 10);
    if (!std::strcmp(argv[i], "--rev")) rev = std::strtoull(argv[i+1], nullptr, 10);
  }
  DecodeRequest r; r.id = RequestId::from(seq); r.initial_attempt = AttemptId::from(seq);
  r.tenant = TenantId::from(tenant); r.model = ModelId::from(model); r.revision = ModelRevision::from(rev);
  r.sequence = SequenceId::from(seq); r.prompt_length = 4; r.max_generation_length = budget;
  r.tenant_weight = 1.0; r.state.id = StateId::from(seq); r.state.estimated_growth = 64;
  TcpConnection c;
  if (!c.connect(host, port).ok()) { std::fprintf(stderr, "submit: connect failed\n"); return 1; }
  if (!c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)).ok()) { std::fprintf(stderr, "submit: send failed\n"); return 1; }
  auto fr = c.recv_frame();
  if (!fr.ok() || fr.value().type != FrameType::SubmitAck) { std::fprintf(stderr, "submit: no ack\n"); return 1; }
  auto a = decode_submit_ack(fr.value().payload);
  if (a.ok()) { std::printf("submitted seq=%llu admitted=%d reason=%s\n", (unsigned long long)a.value().sequence.value(), (int)a.value().admitted, a.value().reason.c_str()); return a.value().admitted ? 0 : 1; }
  return 1;
}

static int cmd_status(int argc, char** argv, bool snapshot) {
  (void)argc;
  std::string host = argv[2]; int port = std::atoi(argv[3]);
  TcpConnection c; if (!c.connect(host, port).ok()) { std::fprintf(stderr, "connect failed\n"); return 1; }
  FrameType req = snapshot ? FrameType::SnapshotRequest : FrameType::StatusQuery;
  if (!c.send_frame(req, {}).ok()) { std::fprintf(stderr, "send failed\n"); return 1; }
  auto fr = c.recv_frame();
  if (!fr.ok()) { std::fprintf(stderr, "recv failed\n"); return 1; }
  if (snapshot && fr.value().type == FrameType::SnapshotReply) {
    auto j = decode_snapshot_reply(fr.value().payload);
    if (j.ok()) { std::printf("%s\n", j.value().c_str()); }
  } else if (fr.value().type == FrameType::StatusReply) {
    auto s = decode_status_reply(fr.value().payload);
    if (s.ok()) {
      std::printf("active=%u ready=%u generated=%llu steps=%llu admitted=%llu stale=%llu epoch=%llu\n",
        s.value().active, s.value().ready, (unsigned long long)s.value().generated_tokens,
        (unsigned long long)s.value().decode_steps, (unsigned long long)s.value().requests_admitted,
        (unsigned long long)s.value().stale_rejections, (unsigned long long)s.value().epoch);
    }
  }
  return 0;
}

static int cmd_cancel(int argc, char** argv) {
  (void)argc;
  std::string host = argv[2]; int port = std::atoi(argv[3]);
  std::uint64_t req = std::strtoull(argv[4], nullptr, 10);
  TcpConnection c; c.connect(host, port);
  QueryRequest q; q.request = RequestId::from(req);
  if (!c.send_frame(FrameType::CancelRequest, encode_query(q)).ok()) { std::fprintf(stderr, "cancel send failed\n"); return 1; }
  auto fr = c.recv_frame(); (void)fr;
  std::printf("cancel request=%llu\n", (unsigned long long)req);
  return 0;
}

static int cmd_explain(int argc, char** argv) {
  (void)argc;
  std::string host = argv[2]; int port = std::atoi(argv[3]);
  std::uint64_t seq = std::strtoull(argv[4], nullptr, 10); std::string q = "why_waiting";
  if (argc > 5) q = argv[5];
  TcpConnection c; c.connect(host, port);
  ExplainRequest er; er.sequence = SequenceId::from(seq); er.question = q;
  if (!c.send_frame(FrameType::ExplainRequest, encode_explain_request(er)).ok()) { std::fprintf(stderr, "explain send failed\n"); return 1; }
  auto fr = c.recv_frame();
  if (fr.ok() && fr.value().type == FrameType::ExplainReply) { auto e = decode_explain_reply(fr.value().payload); if (e.ok()) std::printf("%s\n--json--\n%s\n", e.value().text.c_str(), e.value().json.c_str()); }
  return 0;
}

static int cmd_inspect(int argc, char** argv) {
  (void)argc;
  std::string host = argv[2]; int port = std::atoi(argv[3]);
  std::uint64_t seq = std::strtoull(argv[4], nullptr, 10);
  TcpConnection c; c.connect(host, port);
  GetAuthority g; g.sequence = SequenceId::from(seq);
  if (!c.send_frame(FrameType::GetAuthority, encode_get_authority(g)).ok()) { std::fprintf(stderr, "inspect send failed\n"); return 1; }
  auto fr = c.recv_frame();
  if (fr.ok() && fr.value().type == FrameType::AuthorityReply) { auto a = decode_authority_reply(fr.value().payload); if (a.ok()) std::printf("seq=%llu exists=%d in_flight_epoch=%llu worker=%llu boot=%llu attempt=%llu gen=%llu generated=%llu\n", (unsigned long long)a.value().sequence.value(), (int)a.value().exists, (unsigned long long)a.value().epoch.value(), (unsigned long long)a.value().worker.value(), (unsigned long long)a.value().worker_boot.value(), (unsigned long long)a.value().attempt.value(), (unsigned long long)a.value().generation.value(), (unsigned long long)a.value().generated); }
  return 0;
}

static int cmd_bench(int argc, char** argv) {
  (void)argc;
  std::uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000;
  std::uint64_t budget = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 8;
  FixedClock clk;
  DecodeFabric::Config cfg; cfg.clock = &clk; cfg.group_limits.max_sequences = 256;
  DecodeFabric fab(cfg);
  CpuDecodeExecutor ex(DeviceId::from(1));
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1); w.health = WorkerHealth::Healthy;
  w.device = ex.device(); w.advertised_capacity = 4096;
  CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1); k.backend = BackendKind::CPU; k.device = DeviceId::from(1); k.dtype = DType::F32;
  w.supported_models.push_back(k);
  (void)fab.register_worker(w);
  for (std::uint64_t i = 0; i < n; ++i) {
    DecodeRequest r; r.id = RequestId::from(i + 1); r.initial_attempt = AttemptId::from(i + 1);
    r.tenant = TenantId::from((i % 5) + 1); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
    r.sequence = SequenceId::from(i + 1); r.prompt_length = 4; r.max_generation_length = budget;
    r.tenant_weight = 1.0; r.state.id = StateId::from(i + 1); r.state.estimated_growth = 64;
    (void)fab.submit(r);
  }
  auto t0 = std::chrono::steady_clock::now();
  (void)fab.pump_until_idle(ex, clk.now(), -1);
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  Stats st = fab.stats();
  double sched_rate = (secs > 0) ? (static_cast<double>(st.decode_steps) / secs) : 0.0;
  std::printf("bench: sequences=%llu budget=%llu decode_steps=%llu generated=%llu elapsed=%.3fs scheduler_ops_per_s=%.0f\n",
              (unsigned long long)n, (unsigned long long)budget, (unsigned long long)st.decode_steps,
              (unsigned long long)st.generated_tokens, secs, sched_rate);
  return 0;
}

static int cmd_recover(int argc, char** argv) {
  (void)argc;
  std::string dir = argv[2];
  FilePersistence fp(dir);
  {
    FixedClock clk; DecodeFabric::Config cfg; cfg.clock = &clk;
    DecodeFabric fab(cfg);
    CpuDecodeExecutor ex(DeviceId::from(1));
    WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1); w.health = WorkerHealth::Healthy;
    w.device = ex.device(); w.advertised_capacity = 100;
    CompatibilityKey k; k.model = ModelId::from(1); k.revision = ModelRevision::from(1); k.backend = BackendKind::CPU; k.device = DeviceId::from(1); k.dtype = DType::F32;
    w.supported_models.push_back(k);
    (void)fab.register_worker(w);
    for (std::uint64_t i = 0; i < 3; ++i) {
      DecodeRequest r; r.id = RequestId::from(i + 1); r.initial_attempt = AttemptId::from(i + 1);
      r.tenant = TenantId::from(1); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
      r.sequence = SequenceId::from(i + 1); r.prompt_length = 4; r.max_generation_length = 10;
      r.tenant_weight = 1.0; r.state.id = StateId::from(i + 1); r.state.estimated_growth = 64;
      (void)fab.submit(r);
    }
    (void)fab.pump_once(ex, clk.now());  // one partial step so state is mid-generation
    auto bytes = fab.serialize_state();
    if (bytes.ok()) (void)fp.write("state", bytes.value());
    std::printf("persisted state (%zu bytes) after partial generation\n", bytes.ok() ? bytes.value().size() : 0);
  }
  {
    FixedClock clk; DecodeFabric::Config cfg; cfg.clock = &clk;
    DecodeFabric fab(cfg);
    auto b = fp.read("state");
    if (b.ok()) {
      auto rec = fab.recover_state(b.value());
      std::printf("recovered: %s active=%u ready=%u\n", rec.ok() ? "ok" : rec.error().message.c_str(), fab.active_sequences(), fab.ready_sequences());
    } else {
      std::printf("recover: no persisted state (%s)\n", b.error().message.c_str());
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }
  std::string cmd = argv[1];
  if (cmd == "serve") { int argc2 = argc - 1; char** argv2 = argv + 1; argv2[0] = const_cast<char*>("df-coordinator"); return decodefabric::coordinator_main(argc2, argv2); }
  if (cmd == "worker") return decodefabric::worker_main(argc - 1, argv + 1);
  if (cmd == "submit" && argc >= 7) return cmd_submit(argc, argv);
  if ((cmd == "status" || cmd == "stats") && argc >= 4) return cmd_status(argc, argv, false);
  if (cmd == "snapshot" && argc >= 4) return cmd_status(argc, argv, true);
  if (cmd == "cancel" && argc >= 5) return cmd_cancel(argc, argv);
  if (cmd == "explain" && argc >= 5) return cmd_explain(argc, argv);
  if (cmd == "inspect" && argc >= 5) return cmd_inspect(argc, argv);
  if (cmd == "bench") return cmd_bench(argc, argv);
  if (cmd == "recover" && argc >= 3) return cmd_recover(argc, argv);
  usage(); return 2;
}
