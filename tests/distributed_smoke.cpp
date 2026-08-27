#include "test_framework.hpp"
#include "process_utils.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include <string>
#include <vector>
using namespace decodefabric;
using namespace decodefabric::protocol;
using namespace decodefabric::transport;
namespace {
const int kPort = 47777;
const std::string kCwd = ".";
}
DF_TEST(app_spawn_submit_complete) {
  const std::string tools = tools_dir();
  SpawnedProcess coord = spawn_process(tools + "/df-coordinator.exe", std::to_string(kPort), kCwd);
  SpawnedProcess w = spawn_process(tools + "/df-worker.exe", "127.0.0.1 " + std::to_string(kPort) + " 1 100 1", kCwd);
  CHECK(coord.ok && w.ok);
  std::fprintf(stderr, "M spawned\n");
  if (!coord.ok) return;
  bool admitted = false;
  DecodeRequest r; r.id = RequestId::from(1); r.initial_attempt = AttemptId::from(1);
  r.tenant = TenantId::from(7); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(100); r.prompt_length = 4; r.max_generation_length = 5;
  r.tenant_weight = 1.0; r.latency_class = LatencyClass::Standard; r.state.id = StateId::from(100); r.state.estimated_growth = 64;
  for (int i = 0; i < 200 && !admitted; ++i) {
    TcpConnection c; if (!c.connect("127.0.0.1", kPort).ok()) continue;
    if (!c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)).ok()) continue;
    auto fr = c.recv_frame(); if (fr.ok() && fr.value().type == FrameType::SubmitAck) { auto a = decode_submit_ack(fr.value().payload); if (a.ok() && a.value().admitted) admitted = true; }
  }
  std::fprintf(stderr, "M admitted=%d\n", (int)admitted);
  CHECK(admitted);
  bool done = false; long long gen = 0;
  for (int i = 0; i < 20000 && !done; ++i) {
    TcpConnection c; if (!c.connect("127.0.0.1", kPort).ok()) continue;
    if (!c.send_frame(FrameType::StatusQuery, {}).ok()) continue;
    auto fr = c.recv_frame(); if (fr.ok() && fr.value().type == FrameType::StatusReply) {
      auto s = decode_status_reply(fr.value().payload); if (!s.ok()) continue;
      gen = (long long)s.value().generated_tokens;
      if (s.value().active == 0) { done = true; }
    }
  }
  std::fprintf(stderr, "M done=%d gen=%lld\n", (int)done, gen);
  CHECK(done);
  CHECK(gen == 5);
  coord.kill(); w.kill();
}
