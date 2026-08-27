#include "test_framework.hpp"
#include "process_utils.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/transport.hpp"
#include <chrono>
#include <string>
#include <thread>
#include <vector>
using namespace decodefabric;
using namespace decodefabric::protocol;
using namespace decodefabric::transport;
namespace {
const int kPort = 48231;
const std::string kCwd = ".";
bool submit(int p, const DecodeRequest& r){
  for (int t = 0; t < 200; ++t) {
    TcpConnection c; if (!c.connect("127.0.0.1", p).ok()) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    if (!c.send_frame(FrameType::SubmitRequest, encode_submit_request(r)).ok()) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    auto fr = c.recv_frame(); if (!fr.ok() || fr.value().type != FrameType::SubmitAck) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); continue; }
    auto a = decode_submit_ack(fr.value().payload); return a.ok() && a.value().admitted;  // definitive
  }
  return false;
}
StatusReply status(int p){ StatusReply s; TcpConnection c; if(!c.connect("127.0.0.1",p).ok()) return s; if(!c.send_frame(FrameType::StatusQuery,{}).ok()) return s; auto fr=c.recv_frame(); if(fr.ok()&&fr.value().type==FrameType::StatusReply){auto a=decode_status_reply(fr.value().payload); if(a.ok()) return a.value();} return s; }
AuthorityReply get_auth(int p, SequenceId seq){ AuthorityReply a; TcpConnection c; if(!c.connect("127.0.0.1",p).ok()) return a; GetAuthority g; g.sequence=seq; if(!c.send_frame(FrameType::GetAuthority, encode_get_authority(g)).ok()) return a; auto fr=c.recv_frame(); if(fr.ok()&&fr.value().type==FrameType::AuthorityReply){auto d=decode_authority_reply(fr.value().payload); if(d.ok()) return d.value();} return a; }
void send_exec(int p, DecodeExecutionResult& r){ TcpConnection c; if(c.connect("127.0.0.1",p).ok()){ (void)c.send_frame(FrameType::ExecuteResult, encode_execute_result(r)); (void)c.recv_frame(); } }
std::uint64_t roll(int p){ std::uint64_t e=0; TcpConnection c; if(c.connect("127.0.0.1",p).ok()){ (void)c.send_frame(FrameType::RollEpoch,{}); auto fr=c.recv_frame(); if(fr.ok()&&fr.value().type==FrameType::StatusReply){auto s=decode_status_reply(fr.value().payload); if(s.ok()) e=s.value().epoch;} } return e; }
DecodeRequest mkreq(std::uint64_t rid, std::uint64_t seq, std::uint64_t tenant, std::uint64_t budget){ DecodeRequest r; r.id=RequestId::from(rid); r.initial_attempt=AttemptId::from(rid); r.tenant=TenantId::from(tenant); r.model=ModelId::from(1); r.revision=ModelRevision::from(1); r.sequence=SequenceId::from(seq); r.prompt_length=4; r.max_generation_length=budget; r.tenant_weight=(tenant==7?3.0:(tenant==9?1.0:2.0)); r.state.id=StateId::from(seq); r.state.estimated_growth=64; r.latency_class=LatencyClass::Standard; return r; }
}
DF_TEST(multiprocess_atomic_closure){
  const std::string tools=tools_dir();
  SpawnedProcess coord=spawn_process(tools+"/df-coordinator.exe", std::to_string(kPort), kCwd);
  SpawnedProcess w1=spawn_process(tools+"/df-worker.exe","127.0.0.1 "+std::to_string(kPort)+" 1 100 1",kCwd);
  SpawnedProcess w2=spawn_process(tools+"/df-worker.exe","127.0.0.1 "+std::to_string(kPort)+" 2 200 1",kCwd);
  CHECK(coord.ok&&w1.ok&&w2.ok); if(!coord.ok) return;
  SequenceId long_seq=SequenceId::from(1003);
  bool any=false;
  for (int i = 0; i < 600 && !any; ++i) { any = submit(kPort, mkreq(13, 1003, 30, 40)); if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
  CHECK(any);
  std::uint64_t total_budget=40, admitted=1;
  auto add=[&](std::uint64_t rid,std::uint64_t seq,std::uint64_t tenant,std::uint64_t budget){ bool okk=submit(kPort,mkreq(rid,seq,tenant,budget)); if(okk){++admitted; total_budget+=budget;} return okk; };
  bool ka=add(10,1000,7,3), kb=add(11,1001,9,5), kc=add(12,1002,7,6), kd=add(14,1004,9,4);
  CHECK(ka&&kb&&kc&&kd);
  bool late=false;
  AuthorityReply preserved; bool captured=false;
  for(int i=0;i<30000&&!(captured&&late);++i){ StatusReply s=status(kPort); if(!late&&s.generated_tokens>0){ late=add(15,1005,30,8); } AuthorityReply a=get_auth(kPort,long_seq); if(a.exists&&!captured){ preserved=a; captured=true; } }
  if(!late){ late=add(15,1005,30,8); }
  CHECK(captured); CHECK(late);
  SpawnedProcess* target=(preserved.worker.value()==1)?&w1:&w2; CHECK(target->kill());
  bool reconciled=false; for(int i=0;i<6000&&!reconciled;++i){ AuthorityReply a=get_auth(kPort,long_seq); if(!a.exists) reconciled=true; std::this_thread::sleep_for(std::chrono::milliseconds(4)); } CHECK(reconciled);
  std::uint64_t new_boot=(preserved.worker.value()==1)?1000:2000;
  SpawnedProcess wr=spawn_process(tools+"/df-worker.exe","127.0.0.1 "+std::to_string(kPort)+" "+std::to_string(preserved.worker.value())+" "+std::to_string(new_boot)+" 1",kCwd); CHECK(wr.ok);
  std::uint64_t ep=roll(kPort); CHECK(ep!=0);
  std::uint64_t base=status(kPort).stale_rejections;
  DecodeExecutionResult r; r.worker=preserved.worker; MemberOutcome mo; mo.sequence=preserved.sequence; mo.kind=MemberOutcomeKind::StepSuccessContinue; mo.generated=1; mo.attempt=preserved.attempt; mo.generation=preserved.generation; r.outcomes.push_back(mo);
  r.dispatch_id=preserved.dispatch; r.epoch=preserved.epoch; r.worker_boot=preserved.worker_boot; send_exec(kPort,r); std::uint64_t s1=status(kPort).stale_rejections; CHECK(s1==base+1);
  r.epoch=CoordinatorEpoch::from(ep); r.worker_boot=preserved.worker_boot; send_exec(kPort,r); std::uint64_t s2=status(kPort).stale_rejections; CHECK(s2==s1+1);
  r.worker_boot=WorkerBootId::from(new_boot); mo.attempt=AttemptId::from(preserved.attempt.value()+999); send_exec(kPort,r); std::uint64_t s3=status(kPort).stale_rejections; CHECK(s3==s2+1);
  mo.attempt=preserved.attempt; mo.generation=DecodeGeneration::from(preserved.generation.value()==0?0:preserved.generation.value()-1); send_exec(kPort,r); std::uint64_t s4=status(kPort).stale_rejections; CHECK(s4==s3+1);
  bool done=false; std::uint64_t fgen=0; for(int i=0;i<20000&&!done;++i){ StatusReply s=status(kPort); fgen=s.generated_tokens; if(s.active==0&&s.requests_admitted==admitted) done=true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  CHECK(done); CHECK(fgen==total_budget);
  coord.kill(); w1.kill(); w2.kill(); wr.kill();
}
