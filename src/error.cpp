#include "decodefabric/error.hpp"
#include "decodefabric/state_machine.hpp"
#include "decodefabric/backend.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/executor.hpp"
#include "decodefabric/worker.hpp"
#include "decodefabric/group.hpp"
#include "decodefabric/observability.hpp"
#include "decodefabric/protocol.hpp"
#include <utility>

namespace decodefabric {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NullObject: return "null_object";
    case ErrorCode::NotReady: return "not_ready";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::AlreadyTerminal: return "already_terminal";
    case ErrorCode::UnknownModel: return "unknown_model";
    case ErrorCode::UnknownWorker: return "unknown_worker";
    case ErrorCode::UnknownDevice: return "unknown_device";
    case ErrorCode::UnknownExecutor: return "unknown_executor";
    case ErrorCode::DuplicateId: return "duplicate_id";
    case ErrorCode::DuplicateRequest: return "duplicate_request";
    case ErrorCode::UnknownSequence: return "unknown_sequence";
    case ErrorCode::IncompatibleGroupMembers: return "incompatible_group_members";
    case ErrorCode::NoCompatibleWorker: return "no_compatible_worker";
    case ErrorCode::GroupFull: return "group_full";
    case ErrorCode::ImpossibleMemoryEstimate: return "impossible_memory_estimate";
    case ErrorCode::ReservationOverflow: return "reservation_overflow";
    case ErrorCode::MemoryExhaustion: return "memory_exhaustion";
    case ErrorCode::NoMemoryHeadroom: return "no_memory_headroom";
    case ErrorCode::ReservationConflict: return "reservation_conflict";
    case ErrorCode::ReservationLeak: return "reservation_leak";
    case ErrorCode::Backpressure: return "backpressure";
    case ErrorCode::QueueSaturated: return "queue_saturated";
    case ErrorCode::TenantLimitExceeded: return "tenant_limit_exceeded";
    case ErrorCode::AdmissionRejected: return "admission_rejected";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::AlreadyCancelled: return "already_cancelled";
    case ErrorCode::DeadlineExpired: return "deadline_expired";
    case ErrorCode::StaleCoordinatorEpoch: return "stale_coordinator_epoch";
    case ErrorCode::StaleWorkerBoot: return "stale_worker_boot";
    case ErrorCode::StaleAttempt: return "stale_attempt";
    case ErrorCode::StaleDecodeGeneration: return "stale_decode_generation";
    case ErrorCode::StaleStateGeneration: return "stale_state_generation";
    case ErrorCode::DuplicateCompletion: return "duplicate_completion";
    case ErrorCode::CompletionForCancelled: return "completion_for_cancelled";
    case ErrorCode::CompletionForExpired: return "completion_for_expired";
    case ErrorCode::CompletionForTerminal: return "completion_for_terminal";
    case ErrorCode::SupersededByRetry: return "superseded_by_retry";
    case ErrorCode::RetryBudgetExhausted: return "retry_budget_exhausted";
    case ErrorCode::NonRetryableFailure: return "non_retryable_failure";
    case ErrorCode::RetryableFailure: return "retryable_failure";
    case ErrorCode::WorkerUnavailable: return "worker_unavailable";
    case ErrorCode::WorkerBusy: return "worker_busy";
    case ErrorCode::WorkerLost: return "worker_lost";
    case ErrorCode::ExecutionFailed: return "execution_failed";
    case ErrorCode::BackendError: return "backend_error";
    case ErrorCode::ProtocolMalformed: return "protocol_malformed";
    case ErrorCode::ProtocolOversizedFrame: return "protocol_oversized_frame";
    case ErrorCode::ProtocolTruncatedFrame: return "protocol_truncated_frame";
    case ErrorCode::ProtocolUnknownVersion: return "protocol_unknown_version";
    case ErrorCode::ProtocolUnknownType: return "protocol_unknown_type";
    case ErrorCode::ProtocolInvalidField: return "protocol_invalid_field";
    case ErrorCode::ProtocolEmptyFrame: return "protocol_empty_frame";
    case ErrorCode::PersistenceCorrupt: return "persistence_corrupt";
    case ErrorCode::PersistenceChecksumMismatch: return "persistence_checksum_mismatch";
    case ErrorCode::PersistenceTruncated: return "persistence_truncated";
    case ErrorCode::PersistenceUnknownVersion: return "persistence_unknown_version";
    case ErrorCode::PersistenceIoError: return "persistence_io_error";
    case ErrorCode::PersistenceInvalidField: return "persistence_invalid_field";
    case ErrorCode::Overflow: return "overflow";
    case ErrorCode::Underflow: return "underflow";
    case ErrorCode::BoundedLengthExceeded: return "bounded_length_exceeded";
    case ErrorCode::InternalError: return "internal_error";
    case ErrorCode::NotImplemented: return "not_implemented";
    case ErrorCode::ThreadSafetyViolation: return "thread_safety_violation";
  }
  return "unknown_error_code";
}

const char* to_string(SequenceState state) noexcept {
  switch (state) {
    case SequenceState::Admitted: return "Admitted";
    case SequenceState::Waiting: return "Waiting";
    case SequenceState::Ready: return "Ready";
    case SequenceState::Grouped: return "Grouped";
    case SequenceState::Reserved: return "Reserved";
    case SequenceState::Dispatched: return "Dispatched";
    case SequenceState::Running: return "Running";
    case SequenceState::StepCompleted: return "StepCompleted";
    case SequenceState::ReadyForNextToken: return "ReadyForNextToken";
    case SequenceState::Yielded: return "Yielded";
    case SequenceState::Paused: return "Paused";
    case SequenceState::CancelRequested: return "CancelRequested";
    case SequenceState::Cancelled: return "Cancelled";
    case SequenceState::DeadlineExpired: return "DeadlineExpired";
    case SequenceState::RetryableFailure: return "RetryableFailure";
    case SequenceState::NonRetryableFailure: return "NonRetryableFailure";
    case SequenceState::Retrying: return "Retrying";
    case SequenceState::Completed: return "Completed";
    case SequenceState::StaleSuperseded: return "StaleSuperseded";
  }
  return "Unknown";
}

const char* to_string(BackendKind kind) noexcept {
  switch (kind) {
    case BackendKind::CPU: return "cpu";
    case BackendKind::CUDA: return "cuda";
  }
  return "unknown_backend";
}

const char* to_string(DType dtype) noexcept {
  switch (dtype) {
    case DType::Invalid: return "invalid";
    case DType::F32: return "f32";
    case DType::F16: return "f16";
    case DType::BF16: return "bf16";
    case DType::F64: return "f64";
    case DType::I32: return "i32";
    case DType::I64: return "i64";
    case DType::U8: return "u8";
  }
  return "unknown_dtype";
}

const char* to_string(LatencyClass c) noexcept {
  switch (c) {
    case LatencyClass::None: return "none";
    case LatencyClass::RealTime: return "realtime";
    case LatencyClass::Interactive: return "interactive";
    case LatencyClass::Standard: return "standard";
    case LatencyClass::Bulk: return "bulk";
  }
  return "unknown_latency";
}

const char* to_string(MemberOutcomeKind k) noexcept {
  switch (k) {
    case MemberOutcomeKind::StepSuccessContinue: return "StepSuccessContinue";
    case MemberOutcomeKind::StepSuccessTerminal: return "StepSuccessTerminal";
    case MemberOutcomeKind::Yielded: return "Yielded";
    case MemberOutcomeKind::RetryableFailure: return "RetryableFailure";
    case MemberOutcomeKind::NonRetryableFailure: return "NonRetryableFailure";
    case MemberOutcomeKind::Cancelled: return "Cancelled";
    case MemberOutcomeKind::Expired: return "Expired";
    case MemberOutcomeKind::StaleAuthorityRejected: return "StaleAuthorityRejected";
  }
  return "UnknownOutcome";
}

const char* to_string(WorkerHealth h) noexcept {
  switch (h) {
    case WorkerHealth::Unknown: return "unknown";
    case WorkerHealth::Healthy: return "healthy";
    case WorkerHealth::Degraded: return "degraded";
    case WorkerHealth::Failed: return "failed";
  }
  return "unknown_health";
}

const char* to_string(GroupExclusion g) noexcept {
  switch (g) {
    case GroupExclusion::None: return "none";
    case GroupExclusion::KeyIncompatible: return "key_incompatible";
    case GroupExclusion::GroupFull: return "group_full";
    case GroupExclusion::WorkLimitExceeded: return "work_limit_exceeded";
    case GroupExclusion::KvGrowthLimitExceeded: return "kv_growth_limit_exceeded";
    case GroupExclusion::MemoryHeadroomExceeded: return "memory_headroom_exceeded";
    case GroupExclusion::TenantShareExceeded: return "tenant_share_exceeded";
    case GroupExclusion::LatencyClassExceeded: return "latency_class_exceeded";
    case GroupExclusion::DeadlinePressureExceeded: return "deadline_pressure_exceeded";
    case GroupExclusion::AlreadyDispatched: return "already_dispatched";
    case GroupExclusion::NotReady: return "not_ready";
    case GroupExclusion::Terminal: return "terminal";
  }
  return "unknown_group_exclusion";
}

const char* to_string(EventKind k) noexcept {
  switch (k) {
    case EventKind::RequestAdmitted: return "RequestAdmitted";
    case EventKind::RequestRejected: return "RequestRejected";
    case EventKind::SequenceReady: return "SequenceReady";
    case EventKind::SequenceGrouped: return "SequenceGrouped";
    case EventKind::GroupFormed: return "GroupFormed";
    case EventKind::GroupGrew: return "GroupGrew";
    case EventKind::GroupShrank: return "GroupShrank";
    case EventKind::DispatchIssued: return "DispatchIssued";
    case EventKind::StepCompleted: return "StepCompleted";
    case EventKind::SequenceCompleted: return "SequenceCompleted";
    case EventKind::SequenceCancelled: return "SequenceCancelled";
    case EventKind::SequenceExpired: return "SequenceExpired";
    case EventKind::RetryStarted: return "RetryStarted";
    case EventKind::RetryFailed: return "RetryFailed";
    case EventKind::WorkerUp: return "WorkerUp";
    case EventKind::WorkerDown: return "WorkerDown";
    case EventKind::WorkerRestarted: return "WorkerRestarted";
    case EventKind::EpochRolled: return "EpochRolled";
    case EventKind::StaleRejected: return "StaleRejected";
    case EventKind::ReservationGranted: return "ReservationGranted";
    case EventKind::ReservationReleased: return "ReservationReleased";
    case EventKind::DeadlineMiss: return "DeadlineMiss";
    case EventKind::BackpressureApplied: return "BackpressureApplied";
    case EventKind::MemoryReconciled: return "MemoryReconciled";
    case EventKind::Yielding: return "Yielding";
  }
  return "UnknownEvent";
}

}  // namespace decodefabric

namespace decodefabric::protocol {

const char* to_string(FrameType t) noexcept {
  switch (t) {
    case FrameType::Hello: return "Hello";
    case FrameType::HelloAck: return "HelloAck";
    case FrameType::ExecuteRequest: return "ExecuteRequest";
    case FrameType::ExecuteResult: return "ExecuteResult";
    case FrameType::WorkerStatus: return "WorkerStatus";
    case FrameType::WorkerShutdownAck: return "WorkerShutdownAck";
    case FrameType::SubmitRequest: return "SubmitRequest";
    case FrameType::Acknowledge: return "Acknowledge";
    case FrameType::CancelRequest: return "CancelRequest";
    case FrameType::StatusQuery: return "StatusQuery";
    case FrameType::StatusReply: return "StatusReply";
    case FrameType::SnapshotRequest: return "SnapshotRequest";
    case FrameType::SnapshotReply: return "SnapshotReply";
    case FrameType::ExplainRequest: return "ExplainRequest";
    case FrameType::ExplainReply: return "ExplainReply";
    case FrameType::Shutdown: return "Shutdown";
    case FrameType::ShutdownAck: return "ShutdownAck";
    case FrameType::Error: return "Error";
    case FrameType::GetAuthority: return "GetAuthority";
    case FrameType::AuthorityReply: return "AuthorityReply";
    case FrameType::SubmitAck: return "SubmitAck";
    case FrameType::RollEpoch: return "RollEpoch";
  }
  return "UnknownFrame";
}

}  // namespace decodefabric::protocol
