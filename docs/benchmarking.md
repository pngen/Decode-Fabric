# Benchmark methodology

The benchmark harness (benchmarks/df-bench) measures real completed runtime work,
never empty loops. Each run records the exact workload configuration and the
corresponding measured rates. The default sizes are 1,000 / 10,000 / 100,000
active sequences with a generation budget of 8 tokens each.

Workload characteristics (uniform generation lengths in the default run; the
other example programs and tests cover mixed short/long generations, sustained
active populations, frequent join/leave churn, and multiple tenants):

- admission: submit N requests with distinct ids, sequences, and a round-robin
  tenant; measure submissions/second.
- schedule + complete: run schedule()/execute()/apply_completion() until every
  sequence has generated its budget; report authoritative decode steps/second.
- persistence: serialize_state() and recover_state() over the full state; report
  the wall time in microseconds.

Measured quantities are separated from derived/estimated values. The rates are
measured (steady-clock elapsed time over structurally completed work); the
generated-token totals are authoritative. We do not count a step that did not
complete, and we do not attribute causal latency.

## Interpretation caveats

- Throughput is single-host, single-threaded control plane with the CPU executor.
  It is a lower bound on a system using the distributed coordinator/workers.
- Admission drops sharply as N grows because admission serializes on the fabric
  lock (measured: 509k/s at N=1000 vs 31k/s at N=10000).
- Serialize/recover scale roughly linearly with the number of sequences because
  the snapshot is a full-state serialization.
- No test or benchmark uses a timeout; runs are allowed to complete naturally.
