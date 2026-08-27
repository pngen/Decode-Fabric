# Public API guide

All public types live in namespace decodefabric under include/decodefabric/.
Ordinary control flow uses Result<T> / Result<void> and ErrorCode, never
exceptions.

## Core types

- Id<Tag>: strongly typed 64-bit identifiers (RequestId, AttemptId, TenantId,
  ModelId, ModelRevision, AdapterId, SequenceId, DispatchId, DecodeGeneration,
  WorkerId, WorkerBootId, CoordinatorEpoch, ReservationId, DeviceId, ...).
- Result<T> / Result<void> and ErrorCode with a rich set of structured codes.
- Clock / MonotonicClock / FixedClock (injectable deterministic time).
- TokenBudget (overflow-safe generated-token budgets).
- SequenceState / SequenceStateMachine (the decode lifecycle).

## Compatibility & grouping

- CompatibilityKey (deterministic typed key) and CompatibilityDecision
  (explainable match/mismatch).
- GroupLimits (max sequences, work, KV growth, memory headroom, tenant share,
  latency class, deadline pressure).
- DecodeGroup and GroupPlacement.

## Scheduling

- SchedulingComponents (named, inspectable policy contributions) and DecodePlan.
- DecodeFabric: submit, cancel, retry, register_worker, mark_worker_dead,
  roll_epoch, schedule, apply_completion, pump_once, pump_until_idle,
  snapshot, stats, events, explain, in_flight_authority, stale_rejections,
  sequence_generated, sequence_state, serialize_state, recover_state.

## Executors

- DecodeExecutor contract with DecodeExecutionRequest / DecodeExecutionResult /
  MemberOutcome (independent per-member outcomes).
- CpuDecodeExecutor: deterministic stateful CPU decode.
- CudaDecodeExecutor: real CUDA decode (sm_120 / RTX 5090), selected only when
  the CUDA backend is enabled.

## State & memory

- StateDescriptor (KV-state identity, generation, bytes, growth, ownership).
- Reservation and per-device memory accounting with headroom enforcement.

## Distributed

- protocol::Frame / FrameDecoder (framed TCP, strict validation).
- protocol message (de)serialization: execute request/result, submit request,
  status/snapshot/explain/authority/epoch. 64-bit ids are carried as raw 64-bit
  integers, never through floating point.
- transport::TcpConnection / TcpListener (Winsock).
- decoder for the control plane; coordinator_main / worker_main entrypoints.

## Thread-safety

DecodeFabric is internally synchronized and safe for concurrent submission,
scheduling, completion, cancellation, retry, snapshot, and worker updates. No
method blocks on I/O or calls an executor while holding the state lock.
