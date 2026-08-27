#pragma once
#include <decodefabric/fabric.hpp>
#include <decodefabric/cpu_executor.hpp>
#include <decodefabric/clock.hpp>
namespace ex {
inline decodefabric::CompatibilityKey key() {
  decodefabric::CompatibilityKey k;
  k.model = decodefabric::ModelId::from(1);
  k.revision = decodefabric::ModelRevision::from(1);
  k.backend = decodefabric::BackendKind::CPU;
  k.device = decodefabric::DeviceId::from(1);
  k.dtype = decodefabric::DType::F32;
  return k;
}
inline decodefabric::DecodeRequest req(std::uint64_t id, std::uint64_t seq, std::uint64_t tenant,
                                       std::uint64_t budget, double weight = 1.0) {
  decodefabric::DecodeRequest r;
  r.id = decodefabric::RequestId::from(id);
  r.initial_attempt = decodefabric::AttemptId::from(id);
  r.tenant = decodefabric::TenantId::from(tenant);
  r.model = decodefabric::ModelId::from(1);
  r.revision = decodefabric::ModelRevision::from(1);
  r.sequence = decodefabric::SequenceId::from(seq);
  r.prompt_length = 4;
  r.max_generation_length = budget;
  r.tenant_weight = weight;
  r.latency_class = decodefabric::LatencyClass::Standard;
  r.state.id = decodefabric::StateId::from(seq);
  r.state.estimated_growth = 64;
  return r;
}
}  // namespace ex
