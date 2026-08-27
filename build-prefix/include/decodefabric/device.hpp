#pragma once
#include <cstdint>
#include <string>
#include "decodefabric/backend.hpp"
#include "decodefabric/ids.hpp"

namespace decodefabric {

// Describes a concrete device an executor can run decode work on. For a CUDA
// backend this is one CUDA device; for the CPU backend it is a single virtual
// host device. Device identity is a DeviceId and devices are typed by backend.
struct DeviceDescriptor {
  DeviceId id;
  BackendKind backend = BackendKind::CPU;
  std::string name;              // e.g. "NVIDIA GeForce RTX 5090"
  std::uint32_t compute_capability_major = 0;  // e.g. 12 for sm_120
  std::uint32_t compute_capability_minor = 0;  // e.g. 0
  std::uint64_t memory_bytes = 0;              // advertised device memory
  std::uint32_t supported_dtypes = 0;          // bitmask of DType (1<<value)
  std::uint32_t max_groups_concurrent = 0;

  bool valid() const noexcept { return id.is_valid(); }
  bool supports_dtype(DType d) const noexcept;
  std::string to_string() const;
};

}  // namespace decodefabric
