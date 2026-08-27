#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/executor.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/worker.hpp"

namespace decodefabric::protocol {

// Binary message (de)serialization. All 64-bit identities are carried as raw
// 64-bit integers, never through a floating-point/JSON representation, so they
// are preserved losslessly across processes.

// --- ExecuteRequest (coordinator -> worker) ---------------------------------
std::vector<std::uint8_t> encode_execute_request(const DecodeExecutionRequest& req);
Result<DecodeExecutionRequest> decode_execute_request(const std::vector<std::uint8_t>& bytes);

// --- ExecuteResult (worker -> coordinator) ----------------------------------
std::vector<std::uint8_t> encode_execute_result(const DecodeExecutionResult& res);
Result<DecodeExecutionResult> decode_execute_result(const std::vector<std::uint8_t>& bytes);

// --- SubmitRequest (client -> coordinator) ----------------------------------
std::vector<std::uint8_t> encode_submit_request(const DecodeRequest& req);
Result<DecodeRequest> decode_submit_request(const std::vector<std::uint8_t>& bytes);

// --- SubmitAck (coordinator -> client) --------------------------------------
struct SubmitAck {
  bool admitted = false;
  SequenceId sequence;
  std::string reason;
};
std::vector<std::uint8_t> encode_submit_ack(const SubmitAck& a);
Result<SubmitAck> decode_submit_ack(const std::vector<std::uint8_t>& bytes);

// --- StatusReply (coordinator -> client) ------------------------------------
struct StatusReply {
  std::uint32_t active = 0;
  std::uint32_t ready = 0;
  std::uint64_t generated_tokens = 0;
  std::uint64_t decode_steps = 0;
  std::uint32_t groups = 0;
  std::uint64_t requests_admitted = 0;
  std::uint64_t stale_rejections = 0;
  std::uint64_t epoch = 0;
};
std::vector<std::uint8_t> encode_status_reply(const StatusReply& s);
Result<StatusReply> decode_status_reply(const std::vector<std::uint8_t>& bytes);

// --- SnapshotReply (coordinator -> client) ----------------------------------
std::vector<std::uint8_t> encode_snapshot_reply(const std::string& json);
Result<std::string> decode_snapshot_reply(const std::vector<std::uint8_t>& bytes);

// --- RequestId/sequence query ------------------------------------------------
struct QueryRequest {
  RequestId request;
  SequenceId sequence;
  bool want_snapshot = false;
};
std::vector<std::uint8_t> encode_query(const QueryRequest& q);
Result<QueryRequest> decode_query(const std::vector<std::uint8_t>& bytes);

// --- Explain query (coordinator -> client) ----------------------------------
struct ExplainRequest {
  SequenceId sequence;
  std::string question;
};
struct ExplainReply {
  std::string text;
  std::string json;
};
std::vector<std::uint8_t> encode_explain_request(const ExplainRequest& q);
Result<ExplainRequest> decode_explain_request(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_explain_reply(const ExplainReply& r);
Result<ExplainReply> decode_explain_reply(const std::vector<std::uint8_t>& bytes);

// --- Authority query (coordinator -> client) ---------------------------------
// Returns the in-flight dispatch authority for a sequence so a driver can
// preserve a genuine pre-restart artifact for stale replay.
struct GetAuthority {
  SequenceId sequence;
};
struct AuthorityReply {
  bool exists = false;
  SequenceId sequence;
  DispatchId dispatch;
  CoordinatorEpoch epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  AttemptId attempt;
  DecodeGeneration generation;
  std::uint64_t generated = 0;
};
std::vector<std::uint8_t> encode_get_authority(const GetAuthority& g);
Result<GetAuthority> decode_get_authority(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_authority_reply(const AuthorityReply& a);
Result<AuthorityReply> decode_authority_reply(const std::vector<std::uint8_t>& bytes);

}  // namespace decodefabric::protocol
