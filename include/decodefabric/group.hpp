#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/compatibility.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/token.hpp"

namespace decodefabric {

// Hard/soft limits used when forming a decode group. These bound throughput
// packing so that per-token latency and memory headroom are not dominated by
// raw group size.
struct GroupLimits {
  std::uint32_t max_sequences = 64;      // max members in one group
  std::uint64_t max_work_units = 0;      // 0 = unbounded (aggregate active tokens)
  std::uint64_t max_kv_growth_bytes = 0; // 0 = unbounded (est. KV growth)
  std::uint64_t memory_headroom_bytes = 0;  // reserve floor under device capacity
  std::uint32_t max_tenant_share = 0;    // 0 = no per-tenant contribution cap
  LatencyClass max_latency_class = LatencyClass::Bulk;  // upper bound allowed
  std::uint32_t max_deadline_pressure = 0;  // 0 = unbounded urgent members
};

// A formed (or forming) execute-only decode group. A group is always formed
// from sequences that share exactly one CompatibilityKey and satisfy the limits.
struct DecodeGroup {
  DecodeGroupId id;
  CompatibilityKey key;
  std::vector<SequenceId> members;      // homogeneous under key
  TimePoint created_at;
  std::uint64_t estimated_work = 0;        // aggregate active work estimate
  std::uint64_t estimated_kv_growth = 0;   // aggregate est. KV growth for one step
  std::uint32_t member_count() const noexcept { return static_cast<std::uint32_t>(members.size()); }
  bool empty() const noexcept { return members.empty(); }
  std::string to_string() const;
};

// Reason a sequence was kept out of (or removed from) a group.
enum class GroupExclusion : std::uint8_t {
  None = 0,
  KeyIncompatible = 1,
  GroupFull = 2,
  WorkLimitExceeded = 3,
  KvGrowthLimitExceeded = 4,
  MemoryHeadroomExceeded = 5,
  TenantShareExceeded = 6,
  LatencyClassExceeded = 7,
  DeadlinePressureExceeded = 8,
  AlreadyDispatched = 9,
  NotReady = 10,
  Terminal = 11,
};
const char* to_string(GroupExclusion g) noexcept;

// Explainable per-sequence group membership reasoning.
struct GroupPlacement {
  SequenceId sequence;
  bool placed = false;
  GroupExclusion exclusion = GroupExclusion::None;
  std::string reason;
};

}  // namespace decodefabric
