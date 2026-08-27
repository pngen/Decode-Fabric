#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/group.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/reservation.hpp"
#include "decodefabric/state_machine.hpp"
#include "decodefabric/worker.hpp"

namespace decodefabric {

// Event kinds recorded into the bounded event history.
enum class EventKind : std::uint16_t {
  RequestAdmitted = 0,
  RequestRejected = 1,
  SequenceReady = 2,
  SequenceGrouped = 3,
  GroupFormed = 4,
  GroupGrew = 5,
  GroupShrank = 6,
  DispatchIssued = 7,
  StepCompleted = 8,
  SequenceCompleted = 9,
  SequenceCancelled = 10,
  SequenceExpired = 11,
  RetryStarted = 12,
  RetryFailed = 13,
  WorkerUp = 14,
  WorkerDown = 15,
  WorkerRestarted = 16,
  EpochRolled = 17,
  StaleRejected = 18,
  ReservationGranted = 19,
  ReservationReleased = 20,
  DeadlineMiss = 21,
  BackpressureApplied = 22,
  MemoryReconciled = 23,
  Yielding = 24,
};
const char* to_string(EventKind k) noexcept;

// A structured event in the bounded event history.
struct Event {
  EventId id;
  TimePoint time;
  EventKind kind;
  RequestId request;
  SequenceId sequence;
  std::string detail;
  std::string to_string() const;
};

// Cumulative counters. Field comments mark whether a value is measured,
// derived, configured, or estimated — Decode Fabric does not invent causality
// and keeps these categories distinct.
struct Stats {
  // measured
  std::uint64_t requests_admitted = 0;
  std::uint64_t requests_rejected = 0;
  std::uint64_t sequences_started = 0;
  std::uint64_t sequences_completed = 0;
  std::uint64_t sequences_cancelled = 0;
  std::uint64_t sequences_expired = 0;
  std::uint64_t sequences_failed = 0;
  std::uint64_t decode_steps = 0;             // authoritative executed steps
  std::uint64_t generated_tokens = 0;         // committed tokens
  std::uint64_t dispatch_issued = 0;
  Nanoseconds total_active_ns = 0;            // sum of executor active time
  Nanoseconds total_queue_ns = 0;             // sum of queue delay per step
  Nanoseconds total_inter_token_ns = 0;       // sum of inter-token latency
  std::uint64_t cancellations = 0;
  std::uint64_t retries = 0;
  std::uint64_t stale_rejections = 0;
  std::uint64_t deadline_misses = 0;
  std::uint64_t group_formed = 0;
  std::uint64_t group_grew = 0;
  std::uint64_t group_shrank = 0;
  std::uint64_t backpressure_events = 0;

  // per-reason stale rejections (aligned to the index of the ErrorCode enum)
  std::vector<std::uint64_t> stale_by_reason;

  // derived
  double tokens_per_second = 0.0;
  double avg_inter_token_ns = 0.0;
  double avg_queue_delay_ns = 0.0;

  // configured
  std::uint32_t max_active_sequences = 0;
  std::uint64_t tokens_per_cycle_budget = 0;
};

// A point-in-time snapshot of the scheduler/fabric state.
struct Snapshot {
  CoordinatorEpoch epoch;
  std::vector<SequenceState> sequence_states;  // by index? (not used)
  std::uint64_t admitted = 0;
  std::uint64_t active = 0;
  std::uint64_t ready = 0;
  std::uint64_t grouped = 0;
  std::uint64_t running = 0;
  std::uint64_t completed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t expired = 0;
  std::vector<DecodeGroup> groups;
  std::vector<Reservation> reservations;
  std::vector<WorkerDescriptor> workers;
  std::vector<std::string> per_tenant_tokens;  // "tenant=... tokens=..."
  Stats stats;
  std::string to_json() const;
};

}  // namespace decodefabric
