#pragma once
#include <cstdint>
#include <string>
#include "decodefabric/clock.hpp"
#include "decodefabric/error.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/state_desc.hpp"
#include "decodefabric/token.hpp"

namespace decodefabric {

// Latency classes. They select different inter-token latency targets and are a
// first-class input to scheduling: a request marked for a stricter class is
// never allowed to be starved indefinitely by more packable work.
enum class LatencyClass : std::uint8_t {
  None = 0,
  RealTime = 1,
  Interactive = 2,
  Standard = 3,
  Bulk = 4,
};
const char* to_string(LatencyClass c) noexcept;

// A decode request: the authoritative intent to generate up to a bounded number
// of tokens for one sequence using one model. Requests are converted into
// sequences by the fabric; each request has a first (initial) attempt id and a
// lineage that is preserved across retries (which mint new attempts).
struct DecodeRequest {
  RequestId id;
  AttemptId initial_attempt;
  TenantId tenant;
  ModelId model;
  ModelRevision revision;
  AdapterId adapter;              // invalid when no adapter
  SequenceId sequence;

  TokenCount prompt_length = 0;
  TokenCount max_generation_length = 0;  // maximum tokens to generate
  TokenCount pregenerated = 0;           // already-committed tokens (for resume)

  std::uint32_t priority = 0;      // higher = more urgent (tie-break weight)
  double tenant_weight = 1.0;      // proportion of service under contention
  LatencyClass latency_class = LatencyClass::Standard;
  TimePoint deadline;              // absolute deadline; 0 = none
  TimePoint arrival;               // set by the fabric, not the caller

  Nanoseconds per_token_target_ns = 0;   // target inter-token latency (0 = none)
  std::uint64_t estimated_kv_growth_per_token = 0;  // bytes each token adds

  DeviceId device_constraint;      // invalid = any
  std::string sampling_metadata;   // opaque external selection/sampling metadata

  StateDescriptor state;           // externally supplied KV-state identity/ref

  bool valid_request_id() const noexcept {
    return id.is_valid() && sequence.is_valid() && tenant.is_valid() &&
           model.is_valid() && revision.is_valid();
  }

  // Validate callable contract before admission. Does not require a deadline or
  // device constraint; those are optional.
  Result<void> validate() const;
};

}  // namespace decodefabric
