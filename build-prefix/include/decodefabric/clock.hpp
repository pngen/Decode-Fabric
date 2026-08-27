#pragma once
#include <chrono>
#include <cstdint>

namespace decodefabric {

// A monotonic time source in nanoseconds. The scheduler and the runtime use an
// injectable Clock so deterministic tests can supply a scripted fake clock. The
// real clock backends use the OS steady clock and are monotonic: once a value
// is returned, a later read is never smaller.
using Nanoseconds = std::int64_t;

// A point in time on the runtime's monotonic timeline, measured in nanoseconds
// since the start of the clock (clock-relative, not wall-clock).
struct TimePoint {
  Nanoseconds ns = 0;

  TimePoint() = default;
  explicit TimePoint(Nanoseconds v) : ns(v) {}

  bool operator==(const TimePoint&) const = default;
  auto operator<=>(const TimePoint&) const = default;

  TimePoint& operator+=(Nanoseconds d) { ns += d; return *this; }
  TimePoint& operator-=(Nanoseconds d) { ns -= d; return *this; }
};

inline TimePoint operator+(TimePoint t, Nanoseconds d) { t += d; return t; }
inline TimePoint operator-(TimePoint t, Nanoseconds d) { t -= d; return t; }
inline Nanoseconds operator-(TimePoint a, TimePoint b) { return a.ns - b.ns; }

// Abstract monotonic clock.
class Clock {
 public:
  virtual ~Clock() = default;
  // Returns a monotonic time point.
  virtual TimePoint now() const = 0;
};

// Default implementation backed by the OS steady clock (std::chrono::steady_clock).
class MonotonicClock final : public Clock {
 public:
  TimePoint now() const override {
    using namespace std::chrono;
    auto t = steady_clock::now().time_since_epoch();
    return TimePoint{static_cast<Nanoseconds>(duration_cast<nanoseconds>(t).count())};
  }
};

// A clock whose base offset is fixed, so tests can control time flow.
class FixedClock final : public Clock {
 public:
  explicit FixedClock(Nanoseconds start = 0) : current_(start) {}
  void advance(Nanoseconds by) { current_ += by; }
  void set(Nanoseconds value) { current_ = value; }
  TimePoint now() const override { return TimePoint{current_}; }

 private:
  Nanoseconds current_ = 0;
};

}  // namespace decodefabric
