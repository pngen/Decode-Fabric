# Decode Fabric architecture

## Components

    +--------------------------- DecodeFabric (scheduler/runtime) ----------------------+
    |  sequences  groups  reservations  workers  tenants                               |
    |  state machine   compatibility keys   group limits   memory/CKV governance        |
    |  SchedulingComponents (inspectable)   fairness   budgets   deadlines              |
    +------------------------------------------------------------------------------+
         | schedule() returns Dispatches      ^ apply_completion() consumes results
         v                                    |
    +-----------+     framed TCP     +----------------+
    | coordinator|------------------>| worker process  |---> executor (CPU/CUDA)
    +-----------+                    +----------------+         |
          |                                      |              v
          +---- client (driver) ---- submit/status/explain/snapshot  [DecodeExecutor]

## Thread-safety model

The DecodeFabric state is guarded by an internal shared_mutex. The invariant is
that **no public method performs blocking/network I/O, invokes a DecodeExecutor,
or re-enters a mutation while holding the state lock**. In particular:

- schedule() takes the lock, forms groups, grants reservations, emits Dispatches,
  and releases the lock. The executor is called *outside* the lock.
- apply_completion() takes the lock and transitions sequences exactly once,
  validating all authority tokens before any advance.
- The distributed coordinator's per-connection handler threads never call the
  fabric while holding a socket lock, and the schedule thread never holds the
  fabric lock across a send/recv.

This avoids the classic re-entrancy/deadlock class the scheduler is vulnerable to
(reading then writing the same lock, holding a write guard across a callback,
recursive mutation, inconsistent lock ordering). Those were audited in the design
rather than masked with timeouts.

## Data flow for one authoritative step

1. submit(request) -> AdmissionDecision (creates a sequence in Waiting/Ready).
2. schedule(now): deadline/cancellation boundary processing, then candidate
   selection under the composite policy, compatibility-aware group formation
   under limits, worker placement, memory reservation, and a Dispatch per group.
3. The caller (or the coordinator) sends each Dispatch to a worker as a
   DecodeExecutionRequest over framed TCP.
4. The worker executes one real decode iteration via its DecodeExecutor and
   returns a DecodeExecutionResult with independent per-member outcomes.
5. apply_completion(result): validates epoch/boot/attempt/generation/dispatch,
   rejects stale/duplicate evidence without advancing, and advances each valid
   continuing member exactly one generation (budget, timestamps, KV growth,
   reservation reconciliation) while terminal members leave immediately.

## Continuous batching

Groups are formed fresh each iteration from the current ready set and keyed by a
deterministic CompatibilityKey. Because the ready set changes every iteration,
membership grows and shrinks across iterations. The fabric records
GroupFormed/GroupGrew/GroupShrank events for observability.

## Persistence

serialize_state() writes a versioned, checksummed snapshot of authoritative state
(epoch, per-sequence metadata, generated count, next generation, budget, state
identity/generation, terminal idempotency records). recover_state() verifies the
magic, version, and CRC before mutating, rebuilds sequences, clears in-flight
authority (stale authority is never restored as current), and recomputes derived
counters from the restored sequences.
