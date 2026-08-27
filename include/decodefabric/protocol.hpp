#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/error.hpp"

namespace decodefabric::protocol {

// Fixed frame layout:
//   [0..3]   uint32 total_length    (includes the 12-byte header)
//   [4..7]   uint32 protocol_version
//   [8..11]  uint32 message_type
//   [12..]   payload (total_length - 12 bytes)
// All integers are little-endian on the wire. 64-bit identities are carried
// exclusively as integer fields, never as floating point.
inline constexpr std::uint32_t kHeaderBytes = 12;
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 64u * 1024u * 1024u;  // 64 MiB
inline constexpr std::uint32_t kMinimumFrameBytes = kHeaderBytes;
inline constexpr std::uint32_t kProtocolVersion = 1;

enum class FrameType : std::uint32_t {
  Hello = 1,
  HelloAck = 2,
  ExecuteRequest = 10,
  ExecuteResult = 11,
  WorkerStatus = 12,
  WorkerShutdownAck = 13,
  SubmitRequest = 20,
  Acknowledge = 21,
  CancelRequest = 22,
  StatusQuery = 23,
  StatusReply = 24,
  SnapshotRequest = 25,
  SnapshotReply = 26,
  ExplainRequest = 27,
  ExplainReply = 28,
  Shutdown = 30,
  ShutdownAck = 31,
  Error = 32,
  GetAuthority = 40,
  AuthorityReply = 41,
  SubmitAck = 42,
  RollEpoch = 43,
};

const char* to_string(FrameType t) noexcept;

struct Frame {
  std::uint32_t total_length = 0;
  std::uint32_t version = 0;
  FrameType type = FrameType::Error;
  std::vector<std::uint8_t> payload;

  bool valid() const noexcept {
    return version == kProtocolVersion && total_length >= kMinimumFrameBytes &&
           total_length == kHeaderBytes + payload.size();
  }
};

// Encode a frame given its type and payload.
std::vector<std::uint8_t> encode_frame(FrameType type, const std::vector<std::uint8_t>& payload);

// A streaming decoder. Feed raw bytes; complete and valid frames are emitted.
// Malformed input yields structured errors.
class FrameDecoder {
 public:
  explicit FrameDecoder(std::uint32_t max_frame = kDefaultMaxFrameBytes) : max_frame_(max_frame) {}

  Result<bool> feed(const std::uint8_t* data, std::size_t len, Frame& out);
  Result<bool> feed(const std::vector<std::uint8_t>& data, Frame& out);

  void reset() { buf_.clear(); }
  std::size_t buffered() const noexcept { return buf_.size(); }

 private:
  Result<bool> try_emit(Frame& out);
  std::vector<std::uint8_t> buf_;
  std::uint32_t max_frame_;
};

}  // namespace decodefabric::protocol
