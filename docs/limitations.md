# Proven limitations (and intentional scope boundaries)

These are the documented boundaries of the verified Decode Fabric 1.0.
Capabilities required by the specification that are implemented are not listed
here as limitations; this lists only what is genuinely out of scope or bounded.

- **Cancellation boundary.** Cancellation becomes authoritative at the decode
  step boundary (between explicit decode iterations), not arbitrarily mid-kernel.
  The required safe interruption boundary is between iterations unless a backend
  provides stronger proven semantics.
- **Single verified CUDA device.** The CUDA backend was verified on one device:
  NVIDIA GeForce RTX 5090, compute capability 12.0 (sm_120), CUDA 13.1.80.
  Multi-GPU topologies are not validated.
- **Opaque externally managed KV.** Decode Fabric represents caller-managed or
  executor-managed decode/KV state as typed, opaque descriptors (StateDescriptor)
  with identity, generation, bytes, growth estimate, and ownership tags. It does
  not interpret the payload contents.
- **Decode governance only.** There is no built-in full-model serving, prefill
  governance, reusable-prefix indexing, speculative-token proposal, model-artifact
  caching, or general cluster scheduler. These are explicitly outside scope.
- **No built-in tokenizer / sampler.** Decode Fabric accepts validated
  executor/sampler terminal metadata (EOS, terminal indication) through typed
  results; it does not detect semantic stop strings.
- **Single-host/loopback distributed proof.** The atomic multiprocess closure
  proof runs the coordinator and workers on one host over loopback framed TCP.
  Cross-host networking was not validated.
- **Worker placement scope.** Placement considers compatibility, memory capacity,
  load, sequence-state location, and operator constraints; it is not a generic
  cluster scheduler.
- **Persistence is snapshot-based.** Recovery reconstructs state from a
  versioned, checksummed snapshot and reconciles formerly-running remote work by
  re-queuing; it does not provide a distributed shared-state store.
- **Executor-resident state is process-local.** Committed executor state (CPU
  recurrent vectors, CUDA device buffers) is never persisted. A worker or
  coordinator restart destroys it. Recovery therefore invalidates any pending
  commit grant (a grant from the old epoch/worker boot is never revived) and
  re-dispatches the sequence from its unchanged committed pre-state under fresh
  authority. For the shipped synthetic executor the deterministic
  reconstruction contract is exactly: re-initialize the recurrent state from the
  sequence identity/attempt and step forward the receipt chain from the
  committed pre-state digest. A real model/KV executor must provide its own
  reconstruction contract at the same boundary.
- **State digests are per-backend.** CPU and CUDA produce different digest values
  (different numeric representations). A receipt-chain is only meaningful within
  one backend/device; across a restart the reconstruction contract re-derives it.
- **Transactional extra round trip (distributed).** The real distributed path
  performs prepare -> authorize -> commit -> receipt (a prepare-response and a
  commit-response over framed TCP) rather than a single execute->complete round
  trip. This adds one network round trip per decode step; it is measured rather
  than claimed to be free.
