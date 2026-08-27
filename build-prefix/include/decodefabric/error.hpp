#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace decodefabric {

// Structured error codes. Ordinary flow control uses Result<T> rather than
// exceptions; exceptions are never used as a control-flow mechanism.
enum class ErrorCode : std::uint32_t {
  Ok = 0,

  // Generic / input validation
  InvalidArgument = 1,
  NullObject = 2,
  NotReady = 3,
  Busy = 4,
  AlreadyTerminal = 5,

  // Identity / model / device / worker resolution
  UnknownModel = 10,
  UnknownWorker = 11,
  UnknownDevice = 12,
  UnknownExecutor = 13,
  DuplicateId = 14,
  DuplicateRequest = 15,
  UnknownSequence = 16,

  // Compatibility / grouping
  IncompatibleGroupMembers = 20,
  NoCompatibleWorker = 21,
  GroupFull = 22,

  // Memory / reservations
  ImpossibleMemoryEstimate = 30,
  ReservationOverflow = 31,
  MemoryExhaustion = 32,
  NoMemoryHeadroom = 33,
  ReservationConflict = 34,
  ReservationLeak = 35,

  // Scheduling / admission
  Backpressure = 40,
  QueueSaturated = 41,
  TenantLimitExceeded = 42,
  AdmissionRejected = 43,

  // Cancellation / expiration
  Cancelled = 50,
  AlreadyCancelled = 51,
  DeadlineExpired = 52,

  // Stale-authority rejections (must be distinguishable from one another)
  StaleCoordinatorEpoch = 60,
  StaleWorkerBoot = 61,
  StaleAttempt = 62,
  StaleDecodeGeneration = 63,
  StaleStateGeneration = 64,
  DuplicateCompletion = 65,
  CompletionForCancelled = 66,
  CompletionForExpired = 67,
  CompletionForTerminal = 68,
  SupersededByRetry = 69,

  // Retry
  RetryBudgetExhausted = 70,
  NonRetryableFailure = 71,
  RetryableFailure = 72,

  // Worker / execution
  WorkerUnavailable = 80,
  WorkerBusy = 81,
  WorkerLost = 82,
  ExecutionFailed = 83,
  BackendError = 84,

  // Protocol (framed TCP)
  ProtocolMalformed = 90,
  ProtocolOversizedFrame = 91,
  ProtocolTruncatedFrame = 92,
  ProtocolUnknownVersion = 93,
  ProtocolUnknownType = 94,
  ProtocolInvalidField = 95,
  ProtocolEmptyFrame = 96,

  // Persistence
  PersistenceCorrupt = 100,
  PersistenceChecksumMismatch = 101,
  PersistenceTruncated = 102,
  PersistenceUnknownVersion = 103,
  PersistenceIoError = 104,
  PersistenceInvalidField = 105,

  // Numeric safety
  Overflow = 110,
  Underflow = 111,
  BoundedLengthExceeded = 112,

  // Internal
  InternalError = 120,
  NotImplemented = 121,
  ThreadSafetyViolation = 122,
};

// Human-readable name for an ErrorCode (used by Explain, CLI, and messages).
const char* to_string(ErrorCode code) noexcept;

// A structured error: a code plus a message describing the specific condition.
struct Error {
  ErrorCode code = ErrorCode::InternalError;
  std::string message;

  Error() = default;
  explicit Error(ErrorCode c) : code(c), message(to_string(c)) {}
  Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}

  bool operator==(const Error&) const = default;
};

// Convenience factory.
inline Error err(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

// ---------------------------------------------------------------------------
// Result<T> : a value-or-error, never throwing on the ordinary path.
// ---------------------------------------------------------------------------
template <class T>
class Result {
 public:
  Result() : data_(Error{ErrorCode::InternalError, "default-constructed Result"}) {}

  // Value constructors.
  Result(const T& v) : data_(v) {}
  Result(T&& v) : data_(std::move(v)) {}

  // Error constructors.
  Result(const Error& e) : data_(e) {}
  Result(Error&& e) : data_(std::move(e)) {}

  static Result ok(const T& v) { return Result(v); }
  static Result ok(T&& v) { return Result(std::move(v)); }

  bool has_value() const noexcept { return std::holds_alternative<T>(data_); }
  bool ok() const noexcept { return has_value(); }
  bool is_error() const noexcept { return !has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  // Accessors. value() is only valid when has_value(); otherwise it is a
  // programming error. error() is only valid when is_error().
  const T& value() const { return std::get<T>(data_); }
  T& value() { return std::get<T>(data_); }
  const T& operator*() const { return value(); }
  T& operator*() { return value(); }
  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }

  const Error& error() const { return std::get<Error>(data_); }

 private:
  std::variant<T, Error> data_;
};

// Result<void> : success or error with no value.
template <>
class Result<void> {
 public:
  Result() noexcept : ok_(true) {}
  Result(const Error& e) : ok_(false), err_(e) {}
  Result(Error&& e) : ok_(false), err_(std::move(e)) {}

  static Result success() { return Result<void>(); }

  bool has_value() const noexcept { return ok_; }
  bool ok() const noexcept { return ok_; }
  bool is_error() const noexcept { return !ok_; }
  explicit operator bool() const noexcept { return ok_; }

  const Error& error() const { return err_; }

 private:
  bool ok_ = true;
  Error err_;
};

// Convenience alias for an error-typed Result<T>.
template <class T>
Result<T> failed(ErrorCode code, std::string message) {
  return Result<T>{Error{code, std::move(message)}};
}

}  // namespace decodefabric
