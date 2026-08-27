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
