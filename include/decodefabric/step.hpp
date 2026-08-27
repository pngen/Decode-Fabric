#pragma once
#include <cstdint>
#include "decodefabric/ids.hpp"
#include "decodefabric/token.hpp"

namespace decodefabric {

// Status of one authoritative decode step. Each successful continuing step
// advances exactly one generation.
enum class StepStatus : std::uint8_t {
  Continue = 0,
  Terminal = 1,
  Yielded = 2,
  Failed = 3,
};

// Per-sequence state at the moment a decode step is (re)planned.
struct DecodeStep {
  SequenceId sequence;
  AttemptId attempt;
  DecodeGeneration generation;  // which authoritative generation this step is
  TokenCount current_length;    // prompt + generated so far
  TokenCount generated_tokens;  // tokens already generated at step start
  TokenCount remaining_budget;  // tokens remaining at step start
  TimePoint ready_at;           // when the sequence became ready for this step
  TimePoint last_token_at;      // when the last token completed (0 if none)

  bool operator==(const DecodeStep&) const = default;
};

}  // namespace decodefabric
