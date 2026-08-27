#include "test_framework.hpp"
#include "decodefabric/protocol.hpp"
#include "decodefabric/messages.hpp"
#include <vector>

using namespace decodefabric;
using namespace decodefabric::protocol;

static DecodeExecutionRequest make_exec_req() {
  DecodeExecutionRequest r;
  r.dispatch_id = DispatchId::from(999);
  r.epoch = CoordinatorEpoch::from(3);
  r.worker = WorkerId::from(7);
  r.worker_boot = WorkerBootId::from(42);
  r.reservation_id = 1234;
  r.key.model = ModelId::from(1); r.key.revision = ModelRevision::from(2);
  r.key.backend = BackendKind::CPU; r.key.device = DeviceId::from(1); r.key.dtype = DType::F32;
  r.device.id = DeviceId::from(1); r.device.backend = BackendKind::CPU; r.device.name = "cpu0";
  r.device.compute_capability_major = 12; r.device.compute_capability_minor = 0;
  r.device.memory_bytes = 17179869184ull; r.device.supported_dtypes = 2;
  DecodeMemberSpec m;
  m.sequence = SequenceId::from(100); m.attempt = AttemptId::from(1);
  m.generation = DecodeGeneration::from(5); m.state.id = StateId::from(100);
  m.state.generation = 0; m.state.owner_tag = 9; m.state.estimated_growth = 64;
  m.current_length = 10; m.generated_tokens = 6; m.remaining_budget = 4;
  m.payload = {1, 2, 3};
  r.members.push_back(m);
  r.group_payload = {9, 8, 7};
  r.deadline_hint_ns = 5000;
  return r;
}

DF_TEST(frame_roundtrip) {
  std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
  auto frame = encode_frame(FrameType::ExecuteRequest, payload);
  FrameDecoder dec;
  Frame out;
  CHECK(dec.feed(frame, out).value() == true);
  CHECK(out.type == FrameType::ExecuteRequest);
  CHECK(out.version == kProtocolVersion);
  CHECK(out.payload == payload);
  CHECK(out.valid());
}

DF_TEST(frame_rejects_truncated) {
  auto frame = encode_frame(FrameType::Shutdown, {1, 2, 3});
  // Truncate the payload but keep the length header claiming full size.
  FrameDecoder dec;
  Frame out;
  auto r = dec.feed(std::vector<std::uint8_t>(frame.begin(), frame.begin() + 10), out);
  CHECK(r.value() == false);  // need more bytes, no error yet
}

DF_TEST(frame_rejects_oversized) {
  FrameDecoder dec(16);  // tiny max
  std::vector<std::uint8_t> big(64, 0);
  // Craft a length header claiming 64 bytes.
  big[0] = 64; big[1] = 0; big[2] = 0; big[3] = 0;
  big[4] = static_cast<std::uint8_t>(kProtocolVersion); big[5] = 0; big[6] = 0; big[7] = 0;
  big[8] = static_cast<std::uint8_t>(FrameType::Shutdown); big[9] = 0; big[10] = 0; big[11] = 0;
  Frame out;
  auto r = dec.feed(big, out);
  CHECK(r.is_error());
  CHECK(r.error().code == ErrorCode::ProtocolOversizedFrame);
}

DF_TEST(frame_rejects_unknown_version) {
  std::vector<std::uint8_t> frame(16, 0);
  frame[0] = 16; frame[1] = 0; frame[2] = 0; frame[3] = 0;
  frame[4] = 7; frame[5] = 0; frame[6] = 0; frame[7] = 0;  // bad version (7 != 1)
  frame[8] = static_cast<std::uint8_t>(FrameType::Shutdown); frame[9] = 0; frame[10] = 0; frame[11] = 0;
  FrameDecoder dec; Frame out;
  auto r = dec.feed(frame, out);
  CHECK(r.is_error());
  CHECK(r.error().code == ErrorCode::ProtocolUnknownVersion);
}

DF_TEST(frame_rejects_empty_and_unknown_type) {
  // length < header (truncated/malformed) => malformed.
  std::vector<std::uint8_t> empty(11, 0);
  empty[0] = 11; empty[4] = static_cast<std::uint8_t>(kProtocolVersion);
  FrameDecoder dec; Frame out;
  CHECK(dec.feed(empty, out).error().code == ErrorCode::ProtocolMalformed);

  // Unknown message type (200).
  std::vector<std::uint8_t> unk(16, 0);
  unk[0] = 16; unk[4] = static_cast<std::uint8_t>(kProtocolVersion); unk[8] = 200;
  FrameDecoder dec2; Frame out2;
  CHECK(dec2.feed(unk, out2).error().code == ErrorCode::ProtocolUnknownType);
}

DF_TEST(execute_request_roundtrip) {
  auto req = make_exec_req();
  auto bytes = encode_execute_request(req);
  auto out = decode_execute_request(bytes);
  CHECK(out.ok());
  const auto& d = out.value();
  CHECK(d.dispatch_id == req.dispatch_id);
  CHECK(d.epoch == req.epoch);
  CHECK(d.worker == req.worker);
  CHECK(d.worker_boot == req.worker_boot);
  CHECK(d.reservation_id == req.reservation_id);
  CHECK(d.key.model == req.key.model);
  CHECK(d.key.device == req.key.device);
  CHECK(d.device.name == req.device.name);
  CHECK(d.members.size() == 1);
  CHECK(d.members[0].sequence == SequenceId::from(100));
  CHECK(d.members[0].attempt == AttemptId::from(1));
  CHECK(d.members[0].generation == DecodeGeneration::from(5));
  CHECK(d.members[0].state.owner_tag == 9);
  CHECK(d.members[0].payload == std::vector<std::uint8_t>({1, 2, 3}));
  CHECK(d.group_payload == std::vector<std::uint8_t>({9, 8, 7}));
  CHECK(d.deadline_hint_ns == 5000);
}

DF_TEST(execute_result_roundtrip) {
  auto req = make_exec_req();
  DecodeExecutionResult r;
  r.dispatch_id = req.dispatch_id; r.epoch = req.epoch; r.worker = req.worker;
  r.worker_boot = req.worker_boot; r.group_active_ns = 1500;
  r.group_error = ErrorCode::Ok; r.group_error_message = "";
  MemberOutcome mo;
  mo.sequence = req.members[0].sequence; mo.kind = MemberOutcomeKind::StepSuccessContinue;
  mo.generated = 1; mo.token_identifier = 77; mo.terminal = false;
  mo.retryable = false; mo.active_ns = 1200; mo.kv_bytes_delta = 64; mo.kv_bytes_after = 128;
  mo.attempt = req.members[0].attempt; mo.generation = req.members[0].generation;
  r.outcomes.push_back(mo);
  auto bytes = encode_execute_result(r);
  auto out = decode_execute_result(bytes);
  CHECK(out.ok());
  const auto& d = out.value();
  CHECK(d.dispatch_id == r.dispatch_id);
  CHECK(d.epoch == r.epoch);
  CHECK(d.worker == r.worker);
  CHECK(d.worker_boot == r.worker_boot);
  CHECK(d.group_active_ns == 1500);
  CHECK(d.outcomes.size() == 1);
  CHECK(d.outcomes[0].kind == MemberOutcomeKind::StepSuccessContinue);
  CHECK(d.outcomes[0].token_identifier == 77);
  CHECK(d.outcomes[0].attempt == req.members[0].attempt);
  CHECK(d.outcomes[0].generation == req.members[0].generation);
}

DF_TEST(submit_request_roundtrip) {
  DecodeRequest r;
  r.id = RequestId::from(5); r.initial_attempt = AttemptId::from(1);
  r.tenant = TenantId::from(3); r.model = ModelId::from(1); r.revision = ModelRevision::from(1);
  r.sequence = SequenceId::from(200); r.prompt_length = 4; r.max_generation_length = 12;
  r.pregenerated = 2; r.priority = 3; r.tenant_weight = 2.5;
  r.latency_class = LatencyClass::Interactive; r.deadline = TimePoint(999);
  r.per_token_target_ns = 1000; r.estimated_kv_growth_per_token = 64;
  r.device_constraint = DeviceId::from(1); r.sampling_metadata = "smp";
  r.state.id = StateId::from(200); r.state.owner_tag = 4;
  auto bytes = encode_submit_request(r);
  auto out = decode_submit_request(bytes);
  CHECK(out.ok());
  const auto& d = out.value();
  CHECK(d.id == RequestId::from(5));
  CHECK(d.sequence == SequenceId::from(200));
  CHECK(d.model == ModelId::from(1));
  CHECK(d.max_generation_length == 12);
  CHECK(d.pregenerated == 2);
  CHECK(d.priority == 3);
  CHECK(d.tenant_weight == 2.5);
  CHECK(d.latency_class == LatencyClass::Interactive);
  CHECK(d.deadline.ns == 999);
  CHECK(d.sampling_metadata == "smp");
  CHECK(d.state.id == StateId::from(200));
}

DF_TEST(status_query_roundtrip) {
  StatusReply s; s.active = 4; s.ready = 2; s.generated_tokens = 100; s.decode_steps = 50; s.groups = 1; s.requests_admitted = 4;
  auto b = encode_status_reply(s);
  auto d = decode_status_reply(b);
  CHECK(d.ok());
  CHECK(d.value().active == 4);
  CHECK(d.value().generated_tokens == 100);
}
