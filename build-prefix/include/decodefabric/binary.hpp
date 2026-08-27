#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "decodefabric/clock.hpp"
#include "decodefabric/error.hpp"

namespace decodefabric::binary {

class Writer {
 public:
  Writer() { bytes_.reserve(256); }
  void u8(std::uint8_t v) { bytes_.push_back(v); }
  void u16(std::uint16_t v) {
    bytes_.push_back(static_cast<std::uint8_t>(v & 0x00FF));
    bytes_.push_back(static_cast<std::uint8_t>((v >> 8) & 0x00FF));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0x000000FF));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0x000000FF));
  }
  void s64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void ns(Nanoseconds v) { s64(v); }
  void bytes(const std::uint8_t* p, std::size_t n) { bytes_.insert(bytes_.end(), p, p + n); }
  void bytes(const std::vector<std::uint8_t>& v) {
    u64(static_cast<std::uint64_t>(v.size()));
    bytes(v.data(), v.size());
  }
  void str(const std::string& s) {
    // Length-prefixed string: u64 byte length followed by raw bytes. This keeps
    // the wire format self-describing and bounded.
    u64(static_cast<std::uint64_t>(s.size()));
    bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  }
  const std::vector<std::uint8_t>& data() const noexcept { return bytes_; }
  std::vector<std::uint8_t> take() { return std::move(bytes_); }
  std::size_t size() const noexcept { return bytes_.size(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t>& b) : data_(b) {}
  explicit Reader(const std::uint8_t* p, std::size_t n) : data_(p, p + n) {}

  Result<std::uint8_t> u8() {
    if (pos_ + 1 > data_.size()) return Result<std::uint8_t>{trunc_err()};
    return Result<std::uint8_t>::ok(data_[pos_++]);
  }
  Result<std::uint16_t> u16() {
    if (pos_ + 2 > data_.size()) return Result<std::uint16_t>{trunc_err()};
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return Result<std::uint16_t>::ok(v);
  }
  Result<std::uint32_t> u32() {
    if (pos_ + 4 > data_.size()) return Result<std::uint32_t>{trunc_err()};
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return Result<std::uint32_t>::ok(v);
  }
  Result<std::uint64_t> u64() {
    if (pos_ + 8 > data_.size()) return Result<std::uint64_t>{trunc_err()};
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return Result<std::uint64_t>::ok(v);
  }
  Result<std::int64_t> s64() {
    auto r = u64();
    if (!r.ok()) return Result<std::int64_t>{r.error()};
    return Result<std::int64_t>::ok(static_cast<std::int64_t>(r.value()));
  }
  Result<Nanoseconds> ns() {
    auto r = s64();
    if (!r.ok()) return Result<Nanoseconds>{r.error()};
    return Result<Nanoseconds>::ok(r.value());
  }
  Result<std::vector<std::uint8_t>> bytes() {
    auto lenr = u64();
    if (!lenr.ok()) return Result<std::vector<std::uint8_t>>{lenr.error()};
    std::uint64_t n = lenr.value();
    if (n > static_cast<std::uint64_t>(data_.size()) - pos_)
      return Result<std::vector<std::uint8_t>>{Error{ErrorCode::PersistenceTruncated,
                                                     "truncated byte-vector payload"}};
    if (n > (1ull << 32))
      return Result<std::vector<std::uint8_t>>{Error{ErrorCode::BoundedLengthExceeded,
                                                     "byte vector exceeds bound"}};
    std::vector<std::uint8_t> out(data_.begin() + static_cast<std::int64_t>(pos_),
                                  data_.begin() + static_cast<std::int64_t>(pos_ + n));
    pos_ += static_cast<std::size_t>(n);
    return Result<std::vector<std::uint8_t>>::ok(std::move(out));
  }
  Result<std::string> str() {
    auto r = bytes();
    if (!r.ok()) return Result<std::string>{r.error()};
    return Result<std::string>::ok(std::string(r.value().begin(), r.value().end()));
  }

  std::size_t remaining() const noexcept { return data_.size() - pos_; }
  std::size_t pos() const noexcept { return pos_; }

 private:
  static Error trunc_err() {
    return Error{ErrorCode::PersistenceTruncated, "truncated byte stream"};
  }
  const std::vector<std::uint8_t> data_;
  std::size_t pos_ = 0;
};

}  // namespace decodefabric::binary
