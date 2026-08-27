# Scheduling policy

Decode Fabric does not collapse scheduling into one opaque score. Each ready
sequence is scored by explicitly named, inspectable components:

- readiness: is the sequence eligible now.
- latency_pressure: time since the last token versus the per-token target.
- fairness_deficit: the tenant's service shortfall relative to its weight.
- priority: explicit urgency (higher = more urgent).
- age: starvation age (time since ready).
- deadline_pressure: proximity to the absolute deadline.
- budget_ratio: remaining generation budget fraction.

These are combined into a deterministic priority ordering. The composite score is
derived (not a tunable claim); operators can ask why a sequence ran now, waited,
or lost via Explain, and the components are exposed in DecodePlan.

Deterministic tie-breaking uses the sequence id, so scheduling is reproducible
under an injectable Clock.

Continuous batching is a first-class input: a sequence that would pack with a
compatible group is prioritized, but only up to GroupLimits so throughput packing
never dominates per-token latency indefinitely. A sequence waiting for a larger
compatible group is not starved indefinitely: starvation age and deadline
pressure raise its priority so it eventually runs.
