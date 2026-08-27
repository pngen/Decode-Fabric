#include "shared.hpp"
#include <cstdio>
using namespace decodefabric;

// A deterministic executor that fails one specific sequence once, then succeeds.
class FailOnceExecutor final : public DecodeExecutor {
 public:
  explicit FailOnceExecutor(std::uint64_t target) : target_(target), inner_(DeviceId::from(1)) {}
  ExecutorId id() const override { return inner_.id(); }
  BackendKind backend() const override { return inner_.backend(); }
  DeviceDescriptor device() const override { return inner_.device(); }
  bool supports(const CompatibilityKey& key) const override { return inner_.supports(key); }
  Result<DecodeExecutionResult> execute(const DecodeExecutionRequest& req) override {
    auto r = inner_.execute(req);
    if (r.ok()) {
      for (auto& mo : r.value().outcomes) {
        if (!failed_ && mo.sequence.value() == target_) {
          mo.kind = MemberOutcomeKind::RetryableFailure;
          mo.error_code = ErrorCode::RetryableFailure;
          mo.retryable = true;
          failed_ = true;
        }
      }
    }
    return r;
  }
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
