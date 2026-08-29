#include "shared.hpp"
#include <cstdio>
using namespace decodefabric;

// A deterministic executor that fails one specific sequence once, then succeeds.
// It delegates to a real CPU executor but, on the target sequence's first
// prepare, reports a RetryableFailure (a non-commit outcome). The fabric then
// applies retry semantics (a new AttemptId, no rollback of committed tokens) and
// aborts the prepared transition; the executor's committed pre-state is
// unchanged, so the retry re-prepares from the same pre-state.
class FailOnceExecutor final : public DecodeExecutor {
 public:
  explicit FailOnceExecutor(std::uint64_t target) : target_(target), inner_(DeviceId::from(1)) {}
  ExecutorId id() const override { return inner_.id(); }
  BackendKind backend() const override { return inner_.backend(); }
  DeviceDescriptor device() const override { return inner_.device(); }
  bool supports(const CompatibilityKey& key) const override { return inner_.supports(key); }
  Result<PreparedDecode> prepare(const DecodeExecutionRequest& req) override {
    auto r = inner_.prepare(req);
    if (r.ok() && !failed_) {
      for (auto& pm : r.value().members) {
        if (pm.sequence.value() == target_) {
          pm.outcome.kind = MemberOutcomeKind::RetryableFailure;
          pm.outcome.error_code = ErrorCode::RetryableFailure;
          pm.outcome.retryable = true;
          pm.outcome.terminal = false;
          failed_ = true;
        }
      }
    }
    return r;
  }
  Result<MemberReceipt> commit(const CommitGrant& g) override { return inner_.commit(g); }
  Result<void> abort(const AbortPrepared& a) override { return inner_.abort(a); }
 private:
  std::uint64_t target_;
  bool failed_ = false;
  CpuDecodeExecutor inner_;
};

int main() {
  DecodeFabric::Config cfg;
  DecodeFabric fab(cfg);
  FailOnceExecutor ex(70);
  WorkerDescriptor w; w.id = WorkerId::from(1); w.boot_id = WorkerBootId::from(1);
  w.health = WorkerHealth::Healthy; w.device = ex.device(); w.advertised_capacity = 100;
  w.supported_models.push_back(ex::key());
  (void)fab.register_worker(w);
  (void)fab.submit(ex::req(70, 70, 7, 6));
  (void)fab.pump_until_idle(ex, TimePoint(0), 200);
  std::uint64_t retries = fab.stats().retries;
  std::printf("09 retry_after_member_failure: retries=%llu generated=%llu (new AttemptId, no rollback)\n",
              (unsigned long long)retries, (unsigned long long)fab.stats().generated_tokens);
  return (retries >= 1 && fab.stats().generated_tokens == 6) ? 0 : 1;
}
