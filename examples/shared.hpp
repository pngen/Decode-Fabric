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
// Drive one dispatched group through the transactional executor-state
// protocol (prepare -> authorize -> commit/abort -> apply_commit_receipt).
inline void drive(decodefabric::DecodeFabric& fab, decodefabric::DecodeExecutor& dcex,
                  const decodefabric::Dispatch& d) {
  decodefabric::DecodeExecutionRequest q;
  q.dispatch_id = d.id; q.epoch = d.epoch; q.worker = d.worker; q.worker_boot = d.worker_boot;
  q.key = d.key; q.device = d.device; q.reservation_id = d.reservation.value(); q.members = d.members;
  { std::string k = d.key.to_string(); q.group_payload.assign(k.begin(), k.end()); }
  auto prep = dcex.prepare(q);
  if (!prep.ok()) return;
  auto auth = fab.authorize_prepared(prep.value());
  if (!auth.ok()) return;
  decodefabric::ReceiptDecode rd;
  rd.dispatch_id = d.id; rd.epoch = d.epoch; rd.worker = d.worker; rd.worker_boot = d.worker_boot;
  for (const auto& ga : auth.value().members) {
    if (ga.has_grant) { auto cr = dcex.commit(ga.grant); if (cr.ok()) rd.receipts.push_back(cr.value()); }
    else if (ga.has_abort) (void)dcex.abort(ga.abort_spec);
  }
  (void)fab.apply_commit_receipt(rd);
}
}  // namespace ex
