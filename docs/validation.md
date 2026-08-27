# Validation and testing

Decode Fabric uses a dependency-free test harness plus CTest. Configure with
CMake, build, then run ctest from the build directory. No test sets or relies on
a timeout under any circumstances.

## Standard suites

- unit_smoke: version, strong ids, Result/ErrorCode, token budgets, compatibility,
  state-machine transitions, clocks, request validation.
- unit_batching: real continuous batching (group membership changes across
  iterations), mixed generation lengths, a new compatible sequence joining a
  later iteration, tenant fairness, cancellation between steps.
- unit_protocol: frame round-trips; rejection of sub-header/malformed, oversized,
  truncated-while-waiting, unknown-version, and unknown-type frames; lossless
  binary message round-trips for execute request/result, submit, status, explain,
  authority, and epoch.
- unit_authority: 7 specific stale-authority rejection codes (valid advances once,
  duplicate rejected, stale epoch, stale worker boot, stale attempt, stale
  generation, after-cancel rejected).
- unit_persistence: serialize magic/version/round-trip; recovery reconstructs
  active sequences and derived counters; rejection of truncation/corruption.
- unit_robustness: concurrency (multithreaded submission + concurrent read APIs);
  adversarial rejections (duplicate sequence, unknown model, zero budget,
  already-expired deadline, cancel/completion race, oversized frame); randomized
  invariants with a fixed printed seed.
- mp_smoke: a real coordinator OS process + a real worker OS process over real
  framed TCP; a submitted sequence generates its budget and completes.
- mp_closure: the atomic multiprocess closure proof (see below).

## Atomic multiprocess closure proof

The same real scenario (single CTest target, mp_closure) launches a real
coordinator process and two real worker processes, submits a heterogeneous
multi-tenant/mixed-budget workload over the real framed protocol, captures a
genuine pre-restart in-flight dispatch authority, terminates one worker as a real
OS process, restarts it as a new OS process with a new WorkerBootId, rolls the
coordinator epoch, replays four authoritative stale completions (old epoch, old
worker boot, obsolete attempt, obsolete generation) over the real protocol, and
proves each is deterministically rejected with no sequence advance. It then
submits fresh work and proves it resumes and completes, with zero leaked
active/reserved work and no duplicate generation. This is run as part of the
standard 3x Release and 3x Debug suite runs.

## CUDA validation

unit_cuda selects the real device (RTX 5090, sm_120), records a device-memory
baseline, runs real decode iterations on the GPU over a packed workload, verifies
each sequence reaches its budget, and confirms that after teardown device memory
returns to within the driver's caching delta (no per-allocation leak). It is not
reported as passed if the device is missing or an API fails.
