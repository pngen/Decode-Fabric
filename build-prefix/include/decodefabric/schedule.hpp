#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/ids.hpp"
#include "decodefabric/request.hpp"

namespace decodefabric {

// Inspectable scheduling contributions. Decode Fabric deliberately does not
// collapse all scheduling semantics into one opaque score: each policy
// component is named, contributes a value, and is reported so operators can
// ask why a sequence ran now, waited, or lost to another.
struct SchedulingComponents {
  double readiness = 0.0;        // is the sequence ready at all (0/1)
  double latency_pressure = 0.0; // time since last token vs. per-token target
  double fairness_deficit = 0.0; // tenant service vs. weight
  double priority = 0.0;         // explicit priority (higher = more urgent)
  double age = 0.0;              // starvation age (time since ready)
  double deadline_pressure = 0.0;// proximity to deadline (0..1)
  double batch_opportunity = 0.0;// marginal value of packing with this group
  double budget_ratio = 0.0;     // remaining budget fraction
  double backpressure = 0.0;     // 1 when deferred by backpressure

  // The final combined score (derived, not primary).
  double score = 0.0;
};

// Why a sequence was (or was not) chosen now.
struct DecodePlan {
  SequenceId sequence;
  bool scheduled = false;
  std::string primary_reason;       // e.g. "starvation aging" / "deadline pressure"
  std::vector<std::string> factors; // inspectable named factors
  SchedulingComponents components;
  std::string to_string() const;
};

}  // namespace decodefabric
