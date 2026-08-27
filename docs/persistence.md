# Persistence format

serialize_state() and recover_state() implement a versioned, checksummed
snapshot for the authoritative runtime state.

Layout (little-endian, all integers raw):

- magic: 4 bytes "DFST"
- uint32 persistence version (kPersistenceVersion)
- uint64 coordinator epoch
- uint64 next_sequence id, uint64 next_dispatch id
- uint64 sequence count, then per-sequence records:
  sequence id, request id, current attempt id, tenant id, model id, revision id,
  prompt length, max generation length, generated count, generation, generated,
  committed, current length, deadline (ns), priority, latency class, state,
  KV StateDescriptor (id, generation, bytes held, estimated growth, owner tag),
  attempt count.
- uint64 terminal-idempotency record count, then (sequence id, terminal state).
- uint32 CRC-32 of all preceding bytes.

On recovery the fabric verifies magic, version, and CRC before mutating any
state. Truncation, corruption, a bad magic, an unknown version, or a checksum
mismatch is rejected with a structured error and no state is mutated. Non-terminal
sequences are rebuilt; formerly-running remote work is reconciled (in-flight
authority cleared, reservations released, sequence re-queued). Stale worker
authority is never restored as current. Derived counters are recomputed from the
restored sequences. Terminal idempotency records are preserved so duplicates are
rejected after recovery.
