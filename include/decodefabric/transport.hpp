#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "decodefabric/error.hpp"
#include "decodefabric/protocol.hpp"

namespace decodefabric::transport {

// A real framed-TCP transport on Windows (Winsock). Frames use the Decode
// Fabric framed protocol (length + version + type + payload). The transport
// performs blocking I/O; callers that must not block under state locks simply
// never hold those locks during transport operations.
class TcpConnection {
 public:
  TcpConnection();
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&&) noexcept;
  TcpConnection& operator=(TcpConnection&&) noexcept;

  Result<void> connect(const std::string& host, int port);
  Result<void> send_frame(protocol::FrameType type,
                          const std::vector<std::uint8_t>& payload);
  Result<protocol::Frame> recv_frame();

  void close();
  bool valid() const;
  int last_error() const { return last_error_; }
  void* socket_handle() const { return socket_; }
  Result<void> set_nonblocking(bool on);

 private:
  friend class TcpListener;
  void* socket_ = nullptr;   // SOCKET as void*
  std::vector<std::uint8_t> recv_buf_;
  protocol::FrameDecoder decoder_;
  int last_error_ = 0;
  // Serializes sends on this connection. The coordinator's schedule thread may
  // write a dispatch while a per-connection handler thread writes a commit
  // grant; without this the two writes interleave and corrupt the frame stream.
  mutable std::shared_ptr<std::mutex> send_mu_;
};

class TcpListener {
 public:
  TcpListener();
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  Result<void> listen(const std::string& host, int port);
  Result<TcpConnection> accept();
  void close();
  int port() const { return port_; }
  void* socket_handle() const { return socket_; }

 private:
  void* socket_ = nullptr;   // SOCKET as void*
  int port_ = 0;
  bool started_ = false;
  int last_error_ = 0;
};

}  // namespace decodefabric::transport
