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
4. The worker prepares one real decode iteration via its DecodeExecutor and
   returns a PreparedDecode (one PreparedMember per dispatched member).
5. The fabric validates every authority token, then issues a one-use CommitGrant
   per commit-eligible member (or requests an abort / applies a non-commit
   outcome). No canonical generation advances here.
6. The worker commits the authorized tentative transition and returns a
   deterministic MemberReceipt.
7. apply_commit_receipt() finalizes each sequence exactly one generation
   (budget, timestamps, KV growth, reservation reconciliation) only after the
   receipt is validated and its grant is consumed.

## Transactional executor-state protocol

The ordering **executor mutates -> fabric accepts** is deliberately NOT used.
Instead the protocol is:

    executor PREPARES tentative transition
      -> Fabric VALIDATES authority + pre-state digest
      -> Fabric issues one-use COMMIT GRANT
      -> executor COMMIT promotes tentative -> committed (only on the grant)
      -> executor returns a deterministic RECEIPT
      -> Fabric FINALIZES canonical generation/token/budget/current_length

The invariant the runtime enforces mechanically:

> A Decode Fabric generation becomes authoritative only when the exact executor
> transition prepared from the current committed pre-state is authorized under
> current sequence/worker authority, committed exactly once, and bound by a
> receipt whose post-state becomes the next generation's pre-state.

### Executor state model

Each executor holds, per StateId, a **committed** state and (while a transaction
is in flight) a **tentative/prepared** state. A decode execution:

1. reads ONLY the current committed state,
2. computes the proposed next state into tentative storage,
3. leaves the committed state unchanged,
4. returns a prepared result describing the proposed transition,
5. waits for explicit authorization before promoting tentative -> committed,
6. discards tentative state on rejection/abort.

The committed and tentative separation is real: for CPU it is two vectors; for
CUDA it is a committed device buffer plus a separate tentative device buffer
(the committed buffer stays byte-equivalent to its pre-step state until commit).

### Prepared-transition identity

Every prepared transition carries a unique, one-use **proposal id** and is bound
to the complete authority identity: CoordinatorEpoch, WorkerId, WorkerBootId,
SequenceId, StateId, AttemptId, DecodeGeneration, DispatchId, the authoritative
committed-token position, and the pre/post state digests. A commit grant is bound
to this same tuple plus its own one-use grant id, so a grant cannot be replayed
for another sequence, worker boot, epoch, attempt, generation, dispatch, or a
different prepared state.

### State digests

Deterministic state identity. CPU computes a digest over the committed recurrent
state plus the state fields that define execution identity. CUDA computes the
digest over the committed device buffer (bounded host copyback for hashing) plus
the committed token/KV counters. The chain invariant is:

    receipt(G).post_state_digest == prepare(G+1).pre_state_digest

for consecutive accepted generations of the same sequence/attempt lineage.

## Continuous batching

Groups are formed fresh each iteration from the current ready set and keyed by a
deterministic CompatibilityKey. Because the ready set changes every iteration,
membership grows and shrinks across iterations. The fabric records
GroupFormed/GroupGrew/GroupShrank events for observability.

## Persistence

serialize_state() writes a versioned, checksummed snapshot of authoritative state
(epoch, per-sequence metadata, generated count, next generation, budget, state
identity/generation, terminal idempotency records, the committed pre-state digest
per sequence, the grant ledger with consumed receipts, and the receipt chain).
recover_state() verifies the magic, version, and CRC before mutating, rebuilds
sequences, clears in-flight authority (stale authority is never restored as
current), marks any pending grant as aborted, and recomputes derived counters
from the restored sequences.
