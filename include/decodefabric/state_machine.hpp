#pragma once
#include <cstdint>
#include "decodefabric/error.hpp"

namespace decodefabric {

// Lifecycle states for one sequence. A decode sequence is not one indivisible
// task; it progresses through these states across authoritative decode steps.
// The state machine guarantees each successful step advances state at most once
// and that progress is monotonic.
enum class SequenceState : std::uint8_t {
  Admitted = 0,
  Waiting = 1,
  Ready = 2,
  Grouped = 3,
  Reserved = 4,
  Dispatched = 5,
  Running = 6,
  StepCompleted = 7,
  ReadyForNextToken = 8,
  Yielded = 9,
  Paused = 10,
  CancelRequested = 11,
  Cancelled = 12,
  DeadlineExpired = 13,
  RetryableFailure = 14,
  NonRetryableFailure = 15,
  Retrying = 16,
  Completed = 17,
  StaleSuperseded = 18,
};

const char* to_string(SequenceState state) noexcept;

// Whether a state is a terminal (absorbing) state. Terminal sequences never
// leave: they never return to Ready and never receive an authoritative step.
inline bool is_terminal(SequenceState s) noexcept {
  return s == SequenceState::Cancelled || s == SequenceState::DeadlineExpired ||
         s == SequenceState::NonRetryableFailure || s == SequenceState::Completed ||
         s == SequenceState::StaleSuperseded;
}

inline bool is_stopped(SequenceState s) noexcept {
  return s == SequenceState::CancelRequested || is_terminal(s);
}

// A guarded state holder. It is not thread-safe by itself (the owning object
// serializes access); it enforces the *transition* rules so a sequence cannot
// be advanced twice or resurrected after termination.
class SequenceStateMachine {
 public:
  SequenceStateMachine() = default;
  explicit SequenceStateMachine(SequenceState initial) : state_(initial) {}

  SequenceState state() const noexcept { return state_; }

  bool is_active() const noexcept { return !is_terminal(state_); }
  bool is_terminal_state() const noexcept { return is_terminal(state_); }

  // Attempt a transition; returns false (with no change) if the transition is
  // invalid for the current state.
  Result<void> transition_to(SequenceState next);

 private:
  static bool can_transition(SequenceState from, SequenceState to) noexcept;
  SequenceState state_ = SequenceState::Admitted;
};

}  // namespace decodefabric
