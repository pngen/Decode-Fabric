#pragma once
#include <cstdint>
#include "decodefabric/clock.hpp"
#include "decodefabric/ids.hpp"

namespace decodefabric {

enum class ReservationKind : std::uint8_t {
  SequenceReservation = 0,   // holds capacity for one sequence's next step
  GroupReservation = 1,      // holds capacity for a formed group
};

// A memory/device capacity reservation. Reservations are granted before a
// dispatcher, reconciled on completion/cancel/failure, and must never leak or
// double release. Accounting is strictly bounded below by zero.
struct Reservation {
  ReservationId id;
  ReservationKind kind = ReservationKind::SequenceReservation;
  SequenceId sequence;          // for SequenceReservation
  DecodeGroupId group;          // for GroupReservation
  DeviceId device;
  std::uint64_t bytes = 0;      // reserved bytes (est. incremental memory)
  bool released = false;
  bool valid = true;
  TimePoint made_at;

  bool is_released() const noexcept { return released; }
};

}  // namespace decodefabric
