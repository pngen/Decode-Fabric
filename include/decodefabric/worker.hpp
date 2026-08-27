#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/compatibility.hpp"
#include "decodefabric/device.hpp"
#include "decodefabric/ids.hpp"

namespace decodefabric {

enum class WorkerHealth : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Failed = 3,
};
const char* to_string(WorkerHealth h) noexcept;

// A worker descriptor: the logical worker identity plus the boot identity that
// makes a restarted worker distinguishable. Worker authority combines a
// (WorkerId, WorkerBootId) pair; a completion carrying an old boot id for the
// same worker id is stale.
struct WorkerDescriptor {
  WorkerId id;
  WorkerBootId boot_id;           // changes whenever the worker process restarts
  DeviceDescriptor device;
  WorkerHealth health = WorkerHealth::Healthy;
  std::uint64_t advertised_capacity = 0;  // max concurrent decode work units
  std::uint64_t active_reservations = 0;  // current reserved work units
  std::vector<CompatibilityKey> supported_models;  // models this worker can run
  std::uint64_t generation = 0;

  bool valid() const noexcept { return id.is_valid() && boot_id.is_valid(); }
  std::string to_string() const;
};

}  // namespace decodefabric
