#pragma once
#include <compare>
#include <cstdint>
#include <functional>

namespace decodefabric {

// A strongly-typed 64-bit identifier. The Tag template parameter exists purely
// to give distinct C++ types to logically different identifiers and prevent
// accidental cross-category assignment. IDs are always serialized/transported
// as the underlying unsigned 64-bit integer, never through a floating point
// representation, so 64-bit identity is preserved losslessly.
template <class Tag>
class Id {
 public:
  using TagType = Tag;

  constexpr Id() noexcept : value_(0) {}
  constexpr explicit Id(std::uint64_t v) noexcept : value_(v) {}

  // Null id means "none"/unassigned.
  static constexpr Id null() noexcept { return Id{0}; }
  static constexpr Id from(std::uint64_t v) noexcept { return Id{v}; }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_null() const noexcept { return value_ == 0; }
  constexpr bool is_valid() const noexcept { return value_ != 0; }
  constexpr explicit operator bool() const noexcept { return is_valid(); }

  // Lossless textual form (decimal) for logging and CLI.
  std::string to_string() const;

  friend constexpr bool operator==(Id const& a, Id const& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(Id const& a,
                                                     Id const& b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint64_t value_;
};

template <class Tag>
inline std::string Id<Tag>::to_string() const {
  // Keep this definition here so every instantiation has the method; the body
  // is intentionally minimal (no I/O dependency in headers).
  return std::to_string(value_);
}

// Hash support for unordered containers (used for sequence/request maps).
template <class Tag>
struct IdHash {
  std::size_t operator()(Id<Tag> const& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

// --- Identifier categories -------------------------------------------------
struct RequestIdTag;
using RequestId = Id<RequestIdTag>;

struct AttemptIdTag;
using AttemptId = Id<AttemptIdTag>;

struct TenantIdTag;
using TenantId = Id<TenantIdTag>;

struct ModelIdTag;
using ModelId = Id<ModelIdTag>;

struct ModelRevisionTag;
using ModelRevision = Id<ModelRevisionTag>;

struct AdapterIdTag;
using AdapterId = Id<AdapterIdTag>;

struct SequenceIdTag;
using SequenceId = Id<SequenceIdTag>;

struct DecodeGroupIdTag;
using DecodeGroupId = Id<DecodeGroupIdTag>;

struct DispatchIdTag;
using DispatchId = Id<DispatchIdTag>;

struct DecodeGenerationTag;
using DecodeGeneration = Id<DecodeGenerationTag>;

struct StateIdTag;
using StateId = Id<StateIdTag>;

struct DeviceIdTag;
using DeviceId = Id<DeviceIdTag>;

struct WorkerIdTag;
using WorkerId = Id<WorkerIdTag>;

struct WorkerBootIdTag;
using WorkerBootId = Id<WorkerBootIdTag>;

struct CoordinatorEpochTag;
using CoordinatorEpoch = Id<CoordinatorEpochTag>;

struct ReservationIdTag;
using ReservationId = Id<ReservationIdTag>;

struct StepIdTag;
using StepId = Id<StepIdTag>;

struct EventIdTag;
using EventId = Id<EventIdTag>;

struct ExecutorIdTag;
using ExecutorId = Id<ExecutorIdTag>;

struct ClientIdTag;
using ClientId = Id<ClientIdTag>;

struct SessionIdTag;
using SessionId = Id<SessionIdTag>;

}  // namespace decodefabric

// std::hash specializations for the concrete identifier types. These are
// complete specializations of std::hash for program-defined types; each depends
// on the identifier's 64-bit value only.
namespace std {

template <>
struct hash<decodefabric::RequestId> {
  std::size_t operator()(decodefabric::RequestId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::AttemptId> {
  std::size_t operator()(decodefabric::AttemptId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::TenantId> {
  std::size_t operator()(decodefabric::TenantId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::ModelId> {
  std::size_t operator()(decodefabric::ModelId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::ModelRevision> {
  std::size_t operator()(decodefabric::ModelRevision const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::AdapterId> {
  std::size_t operator()(decodefabric::AdapterId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::SequenceId> {
  std::size_t operator()(decodefabric::SequenceId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::DecodeGroupId> {
  std::size_t operator()(decodefabric::DecodeGroupId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::DispatchId> {
  std::size_t operator()(decodefabric::DispatchId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::DecodeGeneration> {
  std::size_t operator()(decodefabric::DecodeGeneration const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::StateId> {
  std::size_t operator()(decodefabric::StateId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::DeviceId> {
  std::size_t operator()(decodefabric::DeviceId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::WorkerId> {
  std::size_t operator()(decodefabric::WorkerId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::WorkerBootId> {
  std::size_t operator()(decodefabric::WorkerBootId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::CoordinatorEpoch> {
  std::size_t operator()(decodefabric::CoordinatorEpoch const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::ReservationId> {
  std::size_t operator()(decodefabric::ReservationId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::StepId> {
  std::size_t operator()(decodefabric::StepId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::EventId> {
  std::size_t operator()(decodefabric::EventId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::ExecutorId> {
  std::size_t operator()(decodefabric::ExecutorId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::ClientId> {
  std::size_t operator()(decodefabric::ClientId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};
template <>
struct hash<decodefabric::SessionId> {
  std::size_t operator()(decodefabric::SessionId const& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};

}  // namespace std
