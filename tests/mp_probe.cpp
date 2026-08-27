#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include <cstdio>
#include <string>
using namespace decodefabric; using namespace decodefabric::protocol; using namespace decodefabric::transport;
int fmain(const char* host, int port) {
  DecodeRequest r; r.id=RequestId::from(1); r.initial_attempt=AttemptId::from(1); r.tenant=TenantId::from(7);
  r.model=ModelId::from(1); r.revision=ModelRevision::from(1); r.sequence=SequenceId::from(100);
  r.prompt_length=4; r.max_generation_length=5; r.tenant_weight=1.0; r.latency_class=LatencyClass::Standard;
  r.state.id=StateId::from(100); r.state.estimated_growth=64;
  for (int i=0;i<100;++i){ TcpConnection c; if(c.connect(host,port).ok()){ c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)); auto fr=c.recv_frame(); if(fr.ok()&&fr.value().type==FrameType::SubmitAck){ auto a=decode_submit_ack(fr.value().payload); if(a.ok()&&a.value().admitted){ fprintf(stderr,"PROBE admitted seq=%llu\n",(unsigned long long)a.value().sequence.value()); break; } } } }
  for (int i=0;i<20000;++i){ TcpConnection c; if(!c.connect(host,port).ok()) continue; c.send_frame(FrameType::StatusQuery,{}); auto fr=c.recv_frame(); if(fr.ok()&&fr.value().type==FrameType::StatusReply){ auto s=decode_status_reply(fr.value().payload); if(s.ok()){ fprintf(stderr,"PROBE active=%u gen=%llu stale=%llu epoch=%llu\n", s.value().active, (unsigned long long)s.value().generated_tokens, (unsigned long long)s.value().stale_rejections, (unsigned long long)s.value().epoch); if(i%2000==0 && s.value().active==0) break; } } }
  return 0;
}
int main(int argc,char**argv){ int p = argc>1?atoi(argv[1]):47777; return fmain("127.0.0.1",p); }
