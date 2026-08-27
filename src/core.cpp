#include "decodefabric/compatibility.hpp"
#include "decodefabric/device.hpp"
#include "decodefabric/worker.hpp"
#include "decodefabric/group.hpp"
#include "decodefabric/schedule.hpp"
#include "decodefabric/observability.hpp"
#include "decodefabric/explain.hpp"
#include "decodefabric/request.hpp"
#include "decodefabric/state_machine.hpp"
#include <sstream>
#include <string>
#include <vector>

namespace decodefabric {

// --- CompatibilityKey -------------------------------------------------------
static void append_u64(std::string& s, std::uint64_t v) {
  if (v == 0) { s += "0"; return; }
  char tmp[24]; int t = 0;
  while (v > 0) { tmp[t++] = static_cast<char>('0' + (v % 10)); v /= 10; }
  for (int i = t - 1; i >= 0; --i) s += tmp[i];
}

bool CompatibilityKey::operator<(const CompatibilityKey& o) const noexcept {
  return to_string() < o.to_string();
}

std::string CompatibilityKey::to_string() const {
  std::string s;
  s.reserve(128);
  s += "model="; append_u64(s, model.value());
  s += ":rev="; append_u64(s, revision.value());
  s += ":adapter="; append_u64(s, adapter.value());
  s += ":backend="; s += ::decodefabric::to_string(backend);
  s += ":device="; append_u64(s, device.value());
  s += ":dtype="; s += ::decodefabric::to_string(dtype);
  s += ":layout="; append_u64(s, tensor_layout);
  s += ":kv="; append_u64(s, kv_representation);
  s += ":seq="; append_u64(s, sequence_requirements);
  s += ":op="; append_u64(s, operator_policy);
  return s;
}

std::uint64_t CompatibilityKey::hash() const noexcept {
  // FNV-1a over the canonical bytes. Deterministic within and across processes.
  const std::string k = to_string();
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : k) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

CompatibilityDecision evaluate_compatibility(const CompatibilityKey& a,
                                             const CompatibilityKey& b) {
  CompatibilityDecision d;
  d.key = a;
  auto add_if = [&d](bool eq, const std::string& name) {
    if (eq) d.matched.push_back(name);
    else d.mismatched.push_back(name);
  };
  add_if(a.model == b.model, "model");
  add_if(a.revision == b.revision, "revision");
  add_if(a.adapter == b.adapter, "adapter");
  add_if(a.backend == b.backend, "backend");
  add_if(a.device == b.device, "device");
  add_if(a.dtype == b.dtype, "dtype");
  add_if(a.tensor_layout == b.tensor_layout, "tensor_layout");
  add_if(a.kv_representation == b.kv_representation, "kv_representation");
  add_if(a.sequence_requirements == b.sequence_requirements, "sequence_requirements");
  add_if(a.operator_policy == b.operator_policy, "operator_policy");

  d.compatible = d.mismatched.empty();
  if (d.compatible) {
    d.reason = "identical compatibility key";
  } else {
    std::string r = "differ in: ";
    for (std::size_t i = 0; i < d.mismatched.size(); ++i) {
      if (i) r += ", ";
      r += d.mismatched[i];
    }
    d.reason = r;
  }
  return d;
}

std::string CompatibilityDecision::to_string() const {
  std::string s = compatible ? "compatible" : "incompatible";
  s += ": " + reason;
  return s;
}

std::string CompatibilityDecision::to_json() const {
  std::string s = "{\"compatible\":";
  s += compatible ? "true" : "false";
  s += ",\"key\":\"" + key.to_string() + "\"";
  s += ",\"reason\":\"" + reason + "\"";
  s += ",\"matched\":[";
  for (std::size_t i = 0; i < matched.size(); ++i) {
    if (i) s += ",";
    s += "\"" + matched[i] + "\"";
  }
  s += "],\"mismatched\":[";
  for (std::size_t i = 0; i < mismatched.size(); ++i) {
    if (i) s += ",";
    s += "\"" + mismatched[i] + "\"";
  }
  s += "]}";
  return s;
}

// --- DeviceDescriptor -------------------------------------------------------
bool DeviceDescriptor::supports_dtype(DType d) const noexcept {
  return (supported_dtypes & (1u << static_cast<std::uint32_t>(d))) != 0;
}

std::string DeviceDescriptor::to_string() const {
  std::ostringstream o;
  o << "device[backend=" << ::decodefabric::to_string(backend)
    << ",name=" << name
    << ",cc=" << compute_capability_major << "." << compute_capability_minor
    << ",mem=" << memory_bytes
    << ",id=" << id.value() << "]";
  return o.str();
}

// --- WorkerDescriptor -------------------------------------------------------
std::string WorkerDescriptor::to_string() const {
  std::ostringstream o;
  o << "worker[id=" << id.value() << ",boot=" << boot_id.value()
    << ",health=" << ::decodefabric::to_string(health)
    << ",cap=" << advertised_capacity << ",resv=" << active_reservations
    << ",dev=" << device.id.value() << "]";
  return o.str();
}

// --- DecodeGroup ------------------------------------------------------------
std::string DecodeGroup::to_string() const {
  std::ostringstream o;
  o << "group[id=" << id.value() << ",members=" << members.size()
    << ",key=" << key.to_string() << ",work=" << estimated_work
    << ",kv=" << estimated_kv_growth << "]";
  return o.str();
}

// --- DecodePlan -------------------------------------------------------------
std::string DecodePlan::to_string() const {
  std::ostringstream o;
  o << "plan[seq=" << sequence.value()
    << ",scheduled=" << (scheduled ? "true" : "false")
    << ",reason=" << primary_reason << "]";
  return o.str();
}

// --- Event ------------------------------------------------------------------
std::string Event::to_string() const {
  std::ostringstream o;
  o << "event[time=" << time.ns << ",kind=" << ::decodefabric::to_string(kind)
    << ",request=" << request.value() << ",sequence=" << sequence.value()
    << ",detail=" << detail << "]";
  return o.str();
}

// --- Snapshot JSON ----------------------------------------------------------
static std::string json_id(std::uint64_t v) {
  std::ostringstream o; o << v; return o.str();
}

std::string Snapshot::to_json() const {
  std::ostringstream o;
  o << "{";
  o << "\"epoch\":" << json_id(epoch.value()) << ",";
  o << "\"admitted\":" << admitted << ",";
  o << "\"active\":" << active << ",";
  o << "\"ready\":" << ready << ",";
  o << "\"grouped\":" << grouped << ",";
  o << "\"running\":" << running << ",";
  o << "\"completed\":" << completed << ",";
  o << "\"cancelled\":" << cancelled << ",";
  o << "\"expired\":" << expired << ",";
  o << "\"groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (i) o << ",";
    o << "{\"id\":" << json_id(groups[i].id.value())
      << ",\"members\":" << groups[i].members.size()
      << ",\"key\":\"" << groups[i].key.to_string() << "\"}";
  }
  o << "],";
  o << "\"reservations\":" << reservations.size() << ",";
  o << "\"workers\":" << workers.size() << ",";
  o << "\"per_tenant_tokens\":[";
  for (std::size_t i = 0; i < per_tenant_tokens.size(); ++i) {
    if (i) o << ",";
    o << "\"" << per_tenant_tokens[i] << "\"";
  }
  o << "],";
  o << "\"stats\":{";
  o << "\"requests_admitted\":" << stats.requests_admitted << ",";
  o << "\"decode_steps\":" << stats.decode_steps << ",";
  o << "\"generated_tokens\":" << stats.generated_tokens << ",";
  o << "\"cancellations\":" << stats.cancellations << ",";
  o << "\"retries\":" << stats.retries << ",";
  o << "\"stale_rejections\":" << stats.stale_rejections << ",";
  o << "\"tokens_per_second\":" << stats.tokens_per_second;
  o << "}}";
  return o.str();
}

// --- Explain JSON -----------------------------------------------------------
std::string Explain::to_json() const {
  std::ostringstream o;
  o << "{";
  o << "\"sequence\":" << json_id(sequence.value()) << ",";
  o << "\"request\":" << json_id(request.value()) << ",";
  o << "\"question\":\"" << question << "\",";
  o << "\"answer\":\"" << answer << "\",";
  o << "\"facts\":[";
  for (std::size_t i = 0; i < facts.size(); ++i) {
    if (i) o << ",";
    o << "\"" << facts[i] << "\"";
  }
  o << "]";
  o << "}";
  return o.str();
}

// --- DecodeRequest::validate ------------------------------------------------
Result<void> DecodeRequest::validate() const {
  if (!valid_request_id()) {
    return Result<void>{Error{ErrorCode::InvalidArgument,
                              "request/sequence/tenant/model/revision id invalid"}};
  }
  if (max_generation_length == 0) {
    return Result<void>{Error{ErrorCode::InvalidArgument,
                              "maximum generation length must be > 0"}};
  }
  if (pregenerated > max_generation_length) {
    return Result<void>{Error{ErrorCode::InvalidArgument,
                              "pregenerated exceeds maximum generation length"}};
  }
  if (!(tenant_weight > 0.0)) {
    return Result<void>{Error{ErrorCode::InvalidArgument,
                              "tenant weight must be positive"}};
  }
  return Result<void>::success();
}

// --- Sequence state machine -------------------------------------------------
bool SequenceStateMachine::can_transition(SequenceState from, SequenceState to) noexcept {
  if (from == to) return false;
  // Terminal states are absorbing.
  if (is_terminal(from)) return false;
  switch (from) {
    case SequenceState::Admitted:
      return to == SequenceState::Waiting || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired;
    case SequenceState::Waiting:
      return to == SequenceState::Ready || to == SequenceState::Paused ||
             to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired;
    case SequenceState::Ready:
      return to == SequenceState::Grouped || to == SequenceState::Waiting ||
             to == SequenceState::Yielded || to == SequenceState::Paused ||
             to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired;
    case SequenceState::ReadyForNextToken:
      return to == SequenceState::Grouped || to == SequenceState::Waiting ||
             to == SequenceState::Yielded || to == SequenceState::Paused ||
             to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired;
    case SequenceState::Grouped:
      return to == SequenceState::Reserved || to == SequenceState::Ready ||
             to == SequenceState::Waiting || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired;
    case SequenceState::Reserved:
      return to == SequenceState::Dispatched || to == SequenceState::Ready ||
             to == SequenceState::Waiting || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired;
    case SequenceState::Dispatched:
      return to == SequenceState::Running || to == SequenceState::StepCompleted ||
             to == SequenceState::CancelRequested || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired || to == SequenceState::StaleSuperseded ||
             to == SequenceState::RetryableFailure || to == SequenceState::NonRetryableFailure ||
             to == SequenceState::Retrying;
    case SequenceState::Running:
      return to == SequenceState::StepCompleted || to == SequenceState::Completed ||
             to == SequenceState::NonRetryableFailure || to == SequenceState::RetryableFailure ||
             to == SequenceState::Retrying || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired || to == SequenceState::StaleSuperseded;
    case SequenceState::StepCompleted:
      return to == SequenceState::ReadyForNextToken || to == SequenceState::Completed ||
             to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired ||
             to == SequenceState::NonRetryableFailure || to == SequenceState::RetryableFailure ||
             to == SequenceState::Retrying || to == SequenceState::StaleSuperseded ||
             to == SequenceState::Waiting;
    case SequenceState::Yielded:
      return to == SequenceState::Ready || to == SequenceState::Waiting ||
             to == SequenceState::Paused || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired;
    case SequenceState::Paused:
      return to == SequenceState::Ready || to == SequenceState::Waiting ||
             to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired;
    case SequenceState::CancelRequested:
      return to == SequenceState::Cancelled || to == SequenceState::DeadlineExpired ||
             to == SequenceState::Completed;
    case SequenceState::RetryableFailure:
      return to == SequenceState::Retrying;
    case SequenceState::Retrying:
      return to == SequenceState::Ready || to == SequenceState::Cancelled ||
             to == SequenceState::DeadlineExpired;
    case SequenceState::Cancelled:
    case SequenceState::DeadlineExpired:
    case SequenceState::NonRetryableFailure:
    case SequenceState::Completed:
    case SequenceState::StaleSuperseded:
      return false;
  }
  return false;
}

Result<void> SequenceStateMachine::transition_to(SequenceState next) {
  if (!can_transition(state_, next)) {
    std::string m = "invalid transition ";
    m += ::decodefabric::to_string(state_);
    m += " -> ";
    m += ::decodefabric::to_string(next);
    return Result<void>{Error{ErrorCode::InvalidArgument, m}};
  }
  state_ = next;
  return Result<void>::success();
}

}  // namespace decodefabric
