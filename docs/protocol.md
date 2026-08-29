# Framed protocol

The control plane uses a fixed framed TCP format:

    [0..3]   uint32 total_length   (includes the 12-byte header)
    [4..7]   uint32 protocol_version
    [8..11]  uint32 message_type
    [12..]   payload (total_length - 12 bytes)

All integers are little-endian on the wire. 64-bit identities are carried as raw
64-bit integers, never through a floating-point/JSON representation, so they are
preserved losslessly across processes.

FrameDecoder validates strictly and rejects:

- sub-header / malformed lengths,
- oversized frames (over the configured maximum),
- truncated frames (waits for more bytes without emitting a bogus frame when the
  length is valid but incomplete),
- unknown protocol versions,
- unknown message types.

Messages are self-describing binary blobs with bounds-checked reads. The message
set includes handshake (Hello), execute request/result (coordinator to/from
worker), submission, status, snapshot, explain, authority query, epoch roll, and
shutdown. A malformed or truncated message yields a structured error and is
never interpreted as a valid field.

Distributed authority carried by a dispatch and validated on completion includes
CoordinatorEpoch, WorkerId, WorkerBootId, RequestId, AttemptId, SequenceId,
DecodeGeneration / StepGeneration, and DispatchId. A completion is accepted only
if all of these are current and consistent; otherwise it is rejected with a
specific stale-authority code and advances nothing.

## Transactional executor-state exchanges

The worker no longer mutates persistent executor state and then reports a result
it forgets. Instead, the distributed path uses a prepare -> authorize -> commit ->
receipt exchange over the same framed transport:

- ExecuteRequest (coordinator -> worker): a prepare request (carries the dispatch
  and its per-member specs).
- PreparedResult (worker -> coordinator): one PreparedMember per member, each with
  a one-use proposal id, pre/post state digests, the committed-position indices,
  and the proposed outcome.
- CommitGrant (coordinator -> worker): a one-use commit grant bound to the full
  authority/proposal tuple (CoordinatorEpoch, WorkerId, WorkerBootId, SequenceId,
  StateId, AttemptId, DecodeGeneration, DispatchId, committed position, pre/post
  digests, proposal id, grant id).
- AbortPrepared (coordinator -> worker): discard the matching prepared transition;
  committed state is unchanged.
- CommitReceipt (worker -> coordinator): deterministic receipts binding the
  committed transition (receipt id, grant id, proposal id, authority tuple,
  committed-token indices before/after, pre/post digests, outcome/terminal).

This adds one network round trip per decode step (prepare and commit responses),
measured rather than claimed to be free. A coordinator restart marks any pending
(never-consumed) grant as aborted; it is never revived. A worker restart with a
new WorkerBootId makes grants from the old boot stale and rejected.

The pre-existing client-facing stale-replay path still accepts a
DecodeExecutionResult over ExecuteResult for the closure proof; such a result is
authority-validated and, being a bare (non-receipt) result, is rejected without
advancing either fabric or executor state.
