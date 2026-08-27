#pragma once
#include <cstdint>
#include <string>
#include "decodefabric/ids.hpp"

namespace decodefabric {

// Reference/order metadata used to validate external decode state. These are
// the caller-supplied identities used by the generator references in
// DecodeRequest and by the executor on each step.
struct StateDescriptor {
  StateId id;                   // identity of the state/sequence KV partition
  std::uint64_t generation = 0; // state generation/version (monotonic per id)
  DeviceId device;              // device/domain the state lives on
  std::uint64_t bytes_held = 0;     // current bytes held
  std::uint64_t estimated_growth = 0;  // estimated growth for the next step
  std::uint64_t owner_tag = 0;    // ownership/reference metadata needed for validity
  std::uint64_t access_version = 0;  // a "generation" that must match for validity

  bool valid() const noexcept { return id.is_valid(); }
  bool equals_identity(const StateDescriptor& o) const noexcept {
    return id == o.id && device == o.device && owner_tag == o.owner_tag;
  }
  // Stale state generation must be rejected by any executor/consumer.
  bool matches_state(const StateDescriptor& o) const noexcept {
    return equals_identity(o) && generation == o.generation &&
           access_version == o.access_version;
  }
};

}  // namespace decodefabric
