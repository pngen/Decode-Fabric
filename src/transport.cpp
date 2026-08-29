#include "decodefabric/transport.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace decodefabric::transport {

static bool winsock_started = false;
static int ensure_winsock() {
  if (winsock_started) return 0;
  WSADATA d;
  int rc = WSAStartup(MAKEWORD(2, 2), &d);
  if (rc == 0) winsock_started = true;
  return rc;
}

static SOCKET from_void(void* p) { return reinterpret_cast<SOCKET>(p); }
static void* to_void(SOCKET s) { return reinterpret_cast<void*>(s); }

TcpConnection::TcpConnection() : socket_(to_void(INVALID_SOCKET)), send_mu_(std::make_shared<std::mutex>()) {}
TcpConnection::~TcpConnection() { close(); }
TcpConnection::TcpConnection(TcpConnection&& o) noexcept
    : socket_(o.socket_), decoder_(std::move(o.decoder_)), recv_buf_(std::move(o.recv_buf_)), last_error_(o.last_error_), send_mu_(std::make_shared<std::mutex>()) {
  o.socket_ = to_void(INVALID_SOCKET);
}
TcpConnection& TcpConnection::operator=(TcpConnection&& o) noexcept {
  if (this != &o) { close(); socket_ = o.socket_; decoder_ = std::move(o.decoder_); recv_buf_ = std::move(o.recv_buf_); last_error_ = o.last_error_; send_mu_ = std::make_shared<std::mutex>(); o.socket_ = to_void(INVALID_SOCKET); }
  return *this;
}
void TcpConnection::close() {
  SOCKET s = from_void(socket_);
  if (s != INVALID_SOCKET) { closesocket(s); socket_ = to_void(INVALID_SOCKET); }
}
bool TcpConnection::valid() const { return from_void(socket_) != INVALID_SOCKET; }

Result<void> TcpConnection::connect(const std::string& host, int port) {
  if (ensure_winsock() != 0) return failed<void>(ErrorCode::BackendError, "WSAStartup failed");
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return failed<void>(ErrorCode::BackendError, "socket() failed");
  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  struct addrinfo* addr = nullptr;
  char portstr[16];
  std::snprintf(portstr, sizeof(portstr), "%d", port);
  if (getaddrinfo(host.c_str(), portstr, &hints, &addr) != 0) { closesocket(s); return failed<void>(ErrorCode::BackendError, "getaddrinfo failed"); }
  int rc = ::connect(s, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
  freeaddrinfo(addr);
  if (rc != 0) { last_error_ = WSAGetLastError(); closesocket(s); return failed<void>(ErrorCode::BackendError, "connect failed"); }
  socket_ = to_void(s);
  return Result<void>::success();
}

Result<void> TcpConnection::set_nonblocking(bool on) {
  SOCKET s = from_void(socket_);
  if (s == INVALID_SOCKET) return failed<void>(ErrorCode::BackendError, "not connected");
  u_long mode = on ? 1 : 0;
  if (ioctlsocket(s, FIONBIO, &mode) != 0) return failed<void>(ErrorCode::BackendError, "ioctlsocket failed");
  return Result<void>::success();
}

Result<void> TcpConnection::send_frame(protocol::FrameType type, const std::vector<std::uint8_t>& payload) {
  std::lock_guard<std::mutex> send_lk(*(send_mu_ ? send_mu_ : (send_mu_ = std::make_shared<std::mutex>())));
  SOCKET s = from_void(socket_);
  if (s == INVALID_SOCKET) return failed<void>(ErrorCode::BackendError, "not connected");
  std::vector<std::uint8_t> frame = protocol::encode_frame(type, payload);
  const char* p = reinterpret_cast<const char*>(frame.data());
  std::size_t n = frame.size();
  while (n > 0) {
    int sent = send(s, p, static_cast<int>(n), 0);
    if (sent == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEINTR) continue;
      last_error_ = WSAGetLastError();
      return failed<void>(ErrorCode::BackendError, "send failed");
    }
    p += sent; n -= static_cast<std::size_t>(sent);
  }
  return Result<void>::success();
}

Result<protocol::Frame> TcpConnection::recv_frame() {
  SOCKET s = from_void(socket_);
  if (s == INVALID_SOCKET) return failed<protocol::Frame>(ErrorCode::BackendError, "not connected");
  char buf[4096];
  protocol::Frame f;
  while (true) {
    Result<bool> fed = decoder_.feed(nullptr, 0, f);
    if (fed.is_error()) return failed<protocol::Frame>(fed.error().code, fed.error().message);
    if (fed.value()) return Result<protocol::Frame>::ok(std::move(f));
    int r = recv(s, buf, sizeof(buf), 0);
    if (r == 0) return failed<protocol::Frame>(ErrorCode::BackendError, "connection closed");
    if (r == SOCKET_ERROR) {
      int e = WSAGetLastError();
      if (e == WSAEINTR) continue;
      last_error_ = e;
      return failed<protocol::Frame>(ErrorCode::BackendError, "recv failed");
    }
    Result<bool> fed2 = decoder_.feed(reinterpret_cast<std::uint8_t*>(buf), static_cast<std::size_t>(r), f);
    if (fed2.is_error()) return failed<protocol::Frame>(fed2.error().code, fed2.error().message);
    if (fed2.value()) return Result<protocol::Frame>::ok(std::move(f));
  }
}

TcpListener::TcpListener() : socket_(to_void(INVALID_SOCKET)) { ensure_winsock(); }
TcpListener::~TcpListener() { close(); }
void TcpListener::close() {
  SOCKET s = from_void(socket_);
  if (s != INVALID_SOCKET) { closesocket(s); socket_ = to_void(INVALID_SOCKET); }
}

Result<void> TcpListener::listen(const std::string& host, int port) {
  if (ensure_winsock() != 0) return failed<void>(ErrorCode::BackendError, "WSAStartup failed");
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return failed<void>(ErrorCode::BackendError, "socket() failed");
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  struct addrinfo hints{};
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
  struct addrinfo* addr = nullptr;
  std::string hoststr = (host.empty() || host == "0.0.0.0" || host == "*") ? "127.0.0.1" : host;
  char portstr[16];
  std::snprintf(portstr, sizeof(portstr), "%d", port);
  if (getaddrinfo(hoststr.c_str(), portstr, &hints, &addr) != 0) { closesocket(s); return failed<void>(ErrorCode::BackendError, "getaddrinfo failed"); }
  if (::bind(s, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) != 0) { last_error_ = WSAGetLastError(); freeaddrinfo(addr); closesocket(s); return failed<void>(ErrorCode::BackendError, "bind failed"); }
  freeaddrinfo(addr);
  if (::listen(s, SOMAXCONN) != 0) { closesocket(s); return failed<void>(ErrorCode::BackendError, "listen failed"); }
  socket_ = to_void(s);
  port_ = port;
  started_ = true;
  return Result<void>::success();
}

Result<TcpConnection> TcpListener::accept() {
  SOCKET s = from_void(socket_);
  if (s == INVALID_SOCKET) return failed<TcpConnection>(ErrorCode::BackendError, "not listening");
  SOCKET c = ::accept(s, nullptr, nullptr);
  if (c == INVALID_SOCKET) return failed<TcpConnection>(ErrorCode::BackendError, "accept failed");
  TcpConnection conn;
  conn.socket_ = to_void(c);
  return Result<TcpConnection>::ok(std::move(conn));
}

}  // namespace decodefabric::transport
