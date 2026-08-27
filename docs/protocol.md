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
