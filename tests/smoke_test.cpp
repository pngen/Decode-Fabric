#include "test_framework.hpp"
#include "decodefabric/version.hpp"
#include "decodefabric/ids.hpp"
#include "decodefabric/error.hpp"
#include "decodefabric/token.hpp"
#include "decodefabric/clock.hpp"
#include "decodefabric/compatibility.hpp"
#include "decodefabric/state_machine.hpp"
#include "decodefabric/request.hpp"

using namespace decodefabric;

DF_TEST(version_is_release) {
  CHECK_EQ(std::string(kVersionString), std::string("1.0.0"));
  CHECK(kVersionMajor == 1 && kVersionMinor == 0 && kVersionPatch == 0);
  CHECK(kProtocolVersion == 1);
}

DF_TEST(strong_ids) {
  RequestId a = RequestId::from(7);
  RequestId b = RequestId::from(7);
  RequestId c = RequestId::from(8);
  CHECK(a == b);
  CHECK(a != c);
  CHECK(a.value() == 7);
  CHECK(a.is_valid());
  CHECK(!a.is_null());
  CHECK(RequestId::null().is_null());
  CHECK(RequestId::from(0).is_null());
  CHECK(a.to_string() == "7");
  // Different ID categories never compare equal at compile time (distinct types).
}

DF_TEST(result_value_and_error) {
  Result<int> ok = Result<int>::ok(42);
  CHECK(ok.ok());
  CHECK(ok.value() == 42);
  Result<int> bad = failed<int>(ErrorCode::InvalidArgument, "nope");
  CHECK(bad.is_error());
  CHECK(bad.error().code == ErrorCode::InvalidArgument);
  CHECK_EQ(bad.error().message, std::string("nope"));

  Result<void> vok = Result<void>::success();
  CHECK(vok.ok());
  Result<void> verr = Result<void>{Error{ErrorCode::NotReady, "x"}};
  CHECK(verr.is_error());
  CHECK_EQ(verr.error().message, std::string("x"));
}

DF_TEST(token_budget) {
  TokenBudget b{5};
  CHECK(b.max() == 5);
  CHECK(b.remaining() == 5);
  CHECK(!b.exhausted());
  CHECK(b.advance().ok());
  CHECK(b.advance().ok());
  CHECK(b.generated() == 2);
  CHECK(b.remaining() == 3);
  b.advance(); b.advance(); b.advance();
  CHECK(b.exhausted());
  CHECK(b.remaining() == 0);
  CHECK(b.advance().is_error());  // exhausted
  CHECK(b.generated() == 5);

  // Overflow safety: advancing near UINT64_MAX cannot wrap.
  TokenBudget big{UINT64_MAX};
  big.advance(UINT64_MAX - 1);
  CHECK(big.generated() == UINT64_MAX - 1);
  CHECK(big.advance().ok());          // reaches max
  CHECK(big.exhausted());
  CHECK(big.advance().is_error());    // would exceed max (reported as backpressure)
}

DF_TEST(compatibility_key) {
  CompatibilityKey a;
  a.model = ModelId::from(1); a.revision = ModelRevision::from(2);
  a.backend = BackendKind::CPU; a.device = DeviceId::from(3);
  a.dtype = DType::F32;
  CompatibilityKey b = a;
  CompatibilityKey c = a;
  c.dtype = DType::F16;
  CHECK(evaluate_compatibility(a, b).compatible);
  auto d = evaluate_compatibility(a, c);
  CHECK(!d.compatible);
  CHECK(d.mismatched.size() == 1);
  CHECK_EQ(d.mismatched[0], std::string("dtype"));
  CHECK(a == b);
  CHECK(a.hash() == b.hash());
  CHECK(a.hash() != c.hash());
  CHECK(a.to_string() == b.to_string());
  CHECK(a.to_string() != c.to_string());
}

DF_TEST(state_machine_transitions) {
  SequenceStateMachine s{SequenceState::Admitted};
  CHECK(s.transition_to(SequenceState::Waiting).ok());
  CHECK(s.transition_to(SequenceState::Ready).ok());
  CHECK(s.transition_to(SequenceState::Grouped).ok());
  CHECK(s.transition_to(SequenceState::Reserved).ok());
  CHECK(s.transition_to(SequenceState::Dispatched).ok());
  CHECK(s.transition_to(SequenceState::Running).ok());
  CHECK(s.transition_to(SequenceState::StepCompleted).ok());
  CHECK(s.transition_to(SequenceState::ReadyForNextToken).ok());
  CHECK(s.transition_to(SequenceState::Grouped).ok());
  CHECK(s.transition_to(SequenceState::Reserved).ok());
  CHECK(s.transition_to(SequenceState::Dispatched).ok());
  CHECK(s.transition_to(SequenceState::Running).ok());
  CHECK(s.transition_to(SequenceState::Completed).ok());
  CHECK(s.is_terminal_state());
  // Terminal is absorbing.
  CHECK(s.transition_to(SequenceState::Ready).is_error());
}

DF_TEST(state_machine_rejects_bad) {
  SequenceStateMachine s{SequenceState::Ready};
  CHECK(s.transition_to(SequenceState::Completed).is_error());  // not allowed
  SequenceStateMachine t{SequenceState::Completed};
  CHECK(t.transition_to(SequenceState::Ready).is_error());
}

DF_TEST(clock_monotonic) {
  MonotonicClock c1;
  TimePoint t1 = c1.now();
  TimePoint t2 = c1.now();
  CHECK(t2 >= t1);
  FixedClock f{1000};
  CHECK(f.now().ns == 1000);
  f.advance(500);
  CHECK(f.now().ns == 1500);
  f.set(10);
  CHECK(f.now().ns == 10);
}

DF_TEST(request_validate) {
  DecodeRequest r;
  r.id = RequestId::from(1);
  r.sequence = SequenceId::from(1);
  r.tenant = TenantId::from(1);
  r.model = ModelId::from(1);
  r.revision = ModelRevision::from(1);
  r.max_generation_length = 10;
  r.tenant_weight = 1.0;
  CHECK(r.validate().ok());
  r.max_generation_length = 0;
  CHECK(r.validate().is_error());
  r.max_generation_length = 10;
  r.pregenerated = 20;
  CHECK(r.validate().is_error());
  r.pregenerated = 0;
  r.tenant_weight = 0.0;
  CHECK(r.validate().is_error());
}
