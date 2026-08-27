#include "shared.hpp"
#include <decodefabric/messages.hpp>
#include <decodefabric/protocol.hpp>
#include <decodefabric/transport.hpp>
#include <cstdio>
#include <cstdlib>
using namespace decodefabric;
using namespace decodefabric::protocol;
using namespace decodefabric::transport;
int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: %s <coordinator_host> <port>\n", argv[0]); return 2; }
  std::string host = argv[1];
  int port = std::atoi(argv[2]);
  DecodeRequest r = ex::req(100, 1000, 7, 5);
  TcpConnection c;
  if (!c.connect(host, port).ok()) { std::printf("12 distributed: connect failed\n"); return 1; }
  if (!c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)).ok()) { std::printf("12 distributed: send failed\n"); return 1; }
  auto fr = c.recv_frame();
  if (!fr.ok() || fr.value().type != FrameType::SubmitAck) { std::printf("12 distributed: no ack\n"); return 1; }
  auto a = decode_submit_ack(fr.value().payload);
  std::printf("12 distributed: submitted seq=%llu admitted=%d\n",
              (unsigned long long)a.value().sequence.value(), a.value().admitted ? 1 : 0);
  TcpConnection c2;
  if (c2.connect(host, port).ok()) { (void)c2.send_frame(FrameType::StatusQuery, {}); auto s = c2.recv_frame(); if (s.ok() && s.value().type == FrameType::StatusReply) { auto st = decode_status_reply(s.value().payload); if (st.ok()) std::printf("12 distributed: active=%u generated=%llu\n", st.value().active, (unsigned long long)st.value().generated_tokens); } }
  return 0;
}
