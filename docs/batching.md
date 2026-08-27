# Continuous batching

Decode Fabric uses iteration-level continuous batching. Active sequences enter
and leave execution groups between decode iterations; the batch is not fixed.

Each scheduling cycle:

1. Deadline and cancellation boundaries are processed.
2. The ready set is the set of sequences in Waiting/Ready/ReadyForNextToken/Yielded.
3. Compatible sequences (same CompatibilityKey) are packed into groups under
   GroupLimits: max sequences, max aggregate work, max estimated KV growth,
   device memory headroom, per-tenant contribution, latency class, deadline
   pressure.
4. Groups are dispatched to workers; returned members are re-queued for the next
   iteration (StepSuccessContinue -> ReadyForNextToken) or leave immediately
   (terminal members).

Group membership therefore changes every iteration: completed sequences leave,
cancelled/expired sequences leave safely, and new compatible sequences join. A
GroupFormed/GroupGrew/GroupShrank event is recorded for observability.

The tests demonstrate real membership change under mixed generation lengths, a
new compatible sequence joining a later iteration, group shrink when members
finish, and continued progress after a group shrinks.
