# Decode Fabric

Decode Fabric is an open-source, vendor-neutral runtime for scheduling, batching,
executing, measuring, and governing iterative token-decoding work across
heterogeneous AI inference infrastructure.

## The governing systems question

> How should active generation work be scheduled, regrouped, executed, and
> governed token by token so that many concurrent sequences make efficient
> progress without sacrificing latency, fairness, memory headroom, cancellation
> responsiveness, or serving throughput?

Decode Fabric answers this by modeling decode as **iterative generation**: a
request is not one indivisible task but a sequence of authoritative decode
steps. Each step is scheduled, packed into a compatible execution group,
dispatched to a worker, executed, and measured. Between iterations the active
set changes: completed/cancelled/expired sequences leave; new ready sequences
join; groups grow and shrink. This is **continuous/iteration-level batching**,
not a static batch.

## What it does (and what it deliberately does not)

Decode Fabric **owns iterative decode execution governance**:

- a rigorous lifecycle state machine for one sequence (Admitted, Waiting, Ready,
  Grouped, Reserved, Dispatched, Running, StepCompleted, ReadyForNextToken,
  Yielded, Paused, CancelRequested, Cancelled, DeadlineExpired, RetryableFailure,
  NonRetryableFailure, Retrying, Completed, StaleSuperseded);
- deterministic continuous batching and compatibility-aware group formation;
- explicit, inspectable scheduling policy components (latency pressure, fairness
  deficit, priority, starvation age, deadline pressure, batch opportunity);
- fairness that accounts for actual token service, not only request counts;
- per-token budgets, deadlines, cancellation at safe step boundaries, and retry
  with a new AttemptId (no implicit rollback of committed steps);
- memory/KV-state governance interfaces (StateDescriptor), reservations with
  per-device accounting, and headroom enforcement;
- worker authority with (WorkerId, WorkerBootId) and coordinator epochs, and
  deterministic stale-authority rejection;
- persistence/recovery with a versioned, checksummed format;
- observability (Snapshot, Stats, bounded Event history) and structured Explain;
- a framed TCP multiprocess control plane (coordinator, workers, client);
- a real deterministic CPU executor and a real CUDA executor (RTX 5090 / sm_120).

It does **not** implement a full model-serving stack. It accepts externally
managed model/KV state through typed interfaces. It does not conflate decode
scheduling with reusable-prefix indexing, full prefill governance, speculative
token proposal, model-artifact caching, or generic resource scheduling. It does
not build a tokenizer or a sampling engine; it accepts validated executor/sampler
terminal metadata through typed results.

## Repository layout

    include/decodefabric/   public API (strong types, Result<T>, scheduler, executors)
    src/                     runtime, protocol, transport, distributed, persistence
    cli/                     decodefabric-cli (serve/worker/submit/status/.../recover/bench)
    tools/                   df-coordinator, df-worker processes
    tests/                   unit, continuous-batching, protocol, authority, persistence, multiprocess
    examples/                compilable examples using the public API
    benchmarks/              real-workload benchmarks
    docs/                    architecture, API, scheduling, batching, persistence, protocol, limitations

## Building

Requirements: CMake >= 3.21, Ninja, MSVC (VS 2022) for C++20, and the CUDA 13.1
toolkit (with an sm_120 device such as the RTX 5090) for the optional CUDA
backend.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

The project compiles with /W4 /WX (MSVC) and -Wall -Wextra -Werror elsewhere.
A single warning anywhere fails the build.

## Install / export

    cmake --install build --prefix <prefix>

An external project can then consume Decode Fabric with:

    find_package(DecodeFabric CONFIG REQUIRED)
    target_link_libraries(my_app PRIVATE DecodeFabric::DecodeFabric)

See examples/external_consumer for a downstream proof that compiles, links,
runs, and exercises real public API behavior.

## CLI

- serve <port> [loops] - run an in-process coordinator.
- worker <host> <port> <id> <boot_id> <device_id> - run a worker process (append --cuda for the CUDA decoder).
- submit <host> <port> <seq> <tenant> <budget> [--model N] [--rev N]
- status / stats / snapshot - query a running coordinator.
- explain <host> <port> <seq> <question> - structured why-answer.
- inspect <host> <port> <seq> - current generation/generated/budget/authority.
- cancel <host> <port> <request_id>
- recover <dir> - persistence round-trip demo.
- bench [sequences] [budget] - real scheduling/completion throughput.

## Example programs

examples/ contains compilable programs: basic CPU decode, repeated iterative
generation, continuous batching, group growth/shrink, mixed generation lengths,
tenant fairness, latency-sensitive decode, cancellation between steps, retry
after member failure, persistence/recovery after partial generation, CUDA decode,
and distributed coordinator/workers.

## External review and transactional authority hardening

After the 1.0.0 release, Micah Zehnder identified an executor-resident state authority boundary in Decode Fabric: canonical Fabric state was protected from stale completions, but CPU/CUDA executor state could still advance before completion authority became final.

That review led to a full transactional hardening pass across CPU, CUDA, Fabric authority, persistence, recovery, and the real distributed control path.

Decode Fabric now uses prepare → authorize → commit semantics with tentative executor state, one-use authority, deterministic pre/post-state digests, and canonical accepted-generation records that serve as durable, idempotent receipts.

If Fabric acceptance exists but executor promotion was not observed, recovery reconciles that same accepted generation exactly once rather than authorizing a second logical transition. Terminal generations are covered as well, and duplicate or replayed acceptance cannot produce a second promotion.

External review credit: **Micah Zehnder** identified the authority boundary that motivated this hardening work.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
