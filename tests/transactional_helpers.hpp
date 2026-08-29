#pragma once

// Reusable transactional driver for tests. Given a fabric + a dispatch, this
// runs the full prepare -> authorize -> commit/abort -> apply_commit_receipt
// cycle for a group, exactly as DecodeFabric::pump_once does for one dispatch.
// It is provided so tests can drive a single (already scheduled) dispatch
// without re-running the scheduler.
#include "decodefabric/fabric.hpp"
#include "decodefabric/executor.hpp"

namespace df_test {

struct DriveResult {
  bool prepared = false;
  bool authorized = false;
  std::size_t committed = 0;    // number of members that produced a receipt
  std::size_t aborted = 0;      // members discarded
  std::size_t rejected = 0;     // stale/authority rejections
};

inline DriveResult drive_dispatch(decodefabric::DecodeFabric& fab,
                                  decodefabric::DecodeExecutor& ex,
                                  const decodefabric::Dispatch& d) {
  DriveResult dr;
  decodefabric::DecodeExecutionRequest req;
  req.dispatch_id = d.id;
  req.epoch = d.epoch;
  req.worker = d.worker;
  req.worker_boot = d.worker_boot;
  req.key = d.key;
  req.device = d.device;
  req.reservation_id = d.reservation.value();
  req.members = d.members;
  { std::string k = d.key.to_string(); req.group_payload.assign(k.begin(), k.end()); }

  auto prep = ex.prepare(req);
  if (!prep.ok()) { dr.prepared = false; return dr; }
  dr.prepared = true;

  auto auth = fab.authorize_prepared(prep.value());
  if (!auth.ok()) { dr.authorized = false; return dr; }
  dr.authorized = true;

  decodefabric::ReceiptDecode rd;
  rd.dispatch_id = d.id;
  rd.epoch = d.epoch;
  rd.worker = d.worker;
  rd.worker_boot = d.worker_boot;
  for (const auto& ga : auth.value().members) {
    if (ga.has_grant) {
      auto cr = ex.commit(ga.grant);
      if (cr.ok()) {
        decodefabric::MemberReceipt mr = std::move(cr.value());
        mr.committed_at = decodefabric::TimePoint(0);
        rd.receipts.push_back(std::move(mr));
        ++dr.committed;
      }
    } else if (ga.has_abort) {
      (void)ex.abort(ga.abort_spec);
      ++dr.aborted;
      if (ga.rejected) ++dr.rejected;
    }
  }
  (void)fab.apply_commit_receipt(rd);
  return dr;
}

}  // namespace df_test
