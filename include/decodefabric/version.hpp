#pragma once
#include <cstdint>

namespace decodefabric {

// Decode Fabric version. This is the library version reported by the
// exported CMake package and by the CLI.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr char const* kVersionString = "1.0.0";

// Protocol version used by the framed TCP control plane and the persistence
// container. Kept independent from the library version so the wire and on-disk
// formats have their own evolution policy.
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kPersistenceVersion = 2;

}  // namespace decodefabric
