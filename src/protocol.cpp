#include "decodefabric/protocol.hpp"
#include "decodefabric/messages.hpp"
#include "decodefabric/binary.hpp"
#include <cstring>

namespace decodefabric::protocol {

using binary::Reader;
using binary::Writer;

// ===========================================================================
// Frame encoding / streaming decode
// ===========================================================================
std::vector<std::uint8_t> encode_frame(FrameType type, const std::vector<std::uint8_t>& payload) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(kHeaderBytes + payload.size()));
  w.u32(kProtocolVersion);
  w.u32(static_cast<std::uint32_t>(type));
  w.bytes(payload.data(), payload.size());
  return w.take();
}

Result<bool> FrameDecoder::try_emit(Frame& out) {
  if (buf_.size() < 4) return Result<bool>::ok(false);
  // Parse length (little-endian at [0..3]).
  std::uint32_t length = static_cast<std::uint32_t>(buf_[0]) |
                         (static_cast<std::uint32_t>(buf_[1]) << 8) |
                         (static_cast<std::uint32_t>(buf_[2]) << 16) |
                         (static_cast<std::uint32_t>(buf_[3]) << 24);
  if (length < kHeaderBytes) {
    buf_.clear();
    return Result<bool>{Error{ErrorCode::ProtocolMalformed, "frame length below header"}};
  }
  if (length > max_frame_) {
    buf_.clear();
    return Result<bool>{Error{ErrorCode::ProtocolOversizedFrame, "frame exceeds max size"}};
  }
  if (buf_.size() < length) return Result<bool>::ok(false);  // need more bytes
  std::uint32_t version = static_cast<std::uint32_t>(buf_[4]) |
                          (static_cast<std::uint32_t>(buf_[5]) << 8) |
                          (static_cast<std::uint32_t>(buf_[6]) << 16) |
                          (static_cast<std::uint32_t>(buf_[7]) << 24);
  if (version != kProtocolVersion) {
    buf_.clear();
    return Result<bool>{Error{ErrorCode::ProtocolUnknownVersion, "unknown protocol version"}};
  }
  std::uint32_t type = static_cast<std::uint32_t>(buf_[8]) |
                       (static_cast<std::uint32_t>(buf_[9]) << 8) |
                       (static_cast<std::uint32_t>(buf_[10]) << 16) |
                       (static_cast<std::uint32_t>(buf_[11]) << 24);
  // Validate message type is a known value.
  bool known = false;
  for (std::uint32_t t = 1; t <= 45; ++t) {
    if (t == type) { known = true; break; }
  }
  if (!known) {
    buf_.clear();
    return Result<bool>{Error{ErrorCode::ProtocolUnknownType, "unknown message type"}};
  }
  out.total_length = length;
  out.version = version;
  out.type = static_cast<FrameType>(type);
  out.payload.assign(buf_.begin() + kHeaderBytes, buf_.begin() + length);
  // Consume the frame.
  buf_.erase(buf_.begin(), buf_.begin() + length);
  return Result<bool>::ok(true);
}

Result<bool> FrameDecoder::feed(const std::uint8_t* data, std::size_t len, Frame& out) {
  buf_.insert(buf_.end(), data, data + len);
  return try_emit(out);
}
Result<bool> FrameDecoder::feed(const std::vector<std::uint8_t>& data, Frame& out) {
  buf_.insert(buf_.end(), data.begin(), data.end());
  return try_emit(out);
}

// ===========================================================================
// Message serialization helpers
// ===========================================================================
static void write_key(Writer& w, const CompatibilityKey& k) {
  w.u64(k.model.value()); w.u64(k.revision.value()); w.u64(k.adapter.value());
  w.u8(static_cast<std::uint8_t>(k.backend));
  w.u64(k.device.value());
  w.u8(static_cast<std::uint8_t>(k.dtype));
  w.u32(k.tensor_layout); w.u32(k.kv_representation);
  w.u32(k.sequence_requirements); w.u32(k.operator_policy);
}
static Result<CompatibilityKey> read_key(Reader& r) {
  CompatibilityKey k;
  auto a = r.u64(); if (!a.ok()) return Result<CompatibilityKey>{a.error()};
  auto b = r.u64(); if (!b.ok()) return Result<CompatibilityKey>{b.error()};
  auto c = r.u64(); if (!c.ok()) return Result<CompatibilityKey>{c.error()};
  auto d = r.u8(); if (!d.ok()) return Result<CompatibilityKey>{d.error()};
  auto e = r.u64(); if (!e.ok()) return Result<CompatibilityKey>{e.error()};
  auto f = r.u8(); if (!f.ok()) return Result<CompatibilityKey>{f.error()};
  auto g = r.u32(); if (!g.ok()) return Result<CompatibilityKey>{g.error()};
  auto h = r.u32(); if (!h.ok()) return Result<CompatibilityKey>{h.error()};
  auto i = r.u32(); if (!i.ok()) return Result<CompatibilityKey>{i.error()};
  auto j = r.u32(); if (!j.ok()) return Result<CompatibilityKey>{j.error()};
  k.model = ModelId::from(a.value()); k.revision = ModelRevision::from(b.value());
  k.adapter = AdapterId::from(c.value()); k.backend = static_cast<BackendKind>(d.value());
  k.device = DeviceId::from(e.value()); k.dtype = static_cast<DType>(f.value());
  k.tensor_layout = g.value(); k.kv_representation = h.value();
  k.sequence_requirements = i.value(); k.operator_policy = j.value();
  return Result<CompatibilityKey>::ok(k);
}
static void write_device(Writer& w, const DeviceDescriptor& d) {
  w.u64(d.id.value()); w.u8(static_cast<std::uint8_t>(d.backend)); w.str(d.name);
  w.u32(d.compute_capability_major); w.u32(d.compute_capability_minor);
  w.u64(d.memory_bytes); w.u32(d.supported_dtypes); w.u32(d.max_groups_concurrent);
}
static Result<DeviceDescriptor> read_device(Reader& r) {
  DeviceDescriptor d;
  auto id = r.u64(); if (!id.ok()) return Result<DeviceDescriptor>{id.error()};
  auto bk = r.u8(); if (!bk.ok()) return Result<DeviceDescriptor>{bk.error()};
  auto nm = r.str(); if (!nm.ok()) return Result<DeviceDescriptor>{nm.error()};
  auto m = r.u32(); if (!m.ok()) return Result<DeviceDescriptor>{m.error()};
  auto mn = r.u32(); if (!mn.ok()) return Result<DeviceDescriptor>{mn.error()};
  auto mem = r.u64(); if (!mem.ok()) return Result<DeviceDescriptor>{mem.error()};
  auto sd = r.u32(); if (!sd.ok()) return Result<DeviceDescriptor>{sd.error()};
  auto mg = r.u32(); if (!mg.ok()) return Result<DeviceDescriptor>{mg.error()};
  d.id = DeviceId::from(id.value()); d.backend = static_cast<BackendKind>(bk.value());
  d.name = nm.value(); d.compute_capability_major = m.value();
  d.compute_capability_minor = mn.value(); d.memory_bytes = mem.value();
  d.supported_dtypes = sd.value(); d.max_groups_concurrent = mg.value();
  return Result<DeviceDescriptor>::ok(d);
}
static void write_state(Writer& w, const StateDescriptor& st) {
  w.u64(st.id.value()); w.u64(st.generation); w.u64(st.device.value());
  w.u64(st.bytes_held); w.u64(st.estimated_growth); w.u64(st.owner_tag); w.u64(st.access_version);
}
static Result<StateDescriptor> read_state(Reader& r) {
  StateDescriptor st;
  auto a = r.u64(); if (!a.ok()) return Result<StateDescriptor>{a.error()};
  auto b = r.u64(); if (!b.ok()) return Result<StateDescriptor>{b.error()};
  auto c = r.u64(); if (!c.ok()) return Result<StateDescriptor>{c.error()};
  auto d = r.u64(); if (!d.ok()) return Result<StateDescriptor>{d.error()};
  auto e = r.u64(); if (!e.ok()) return Result<StateDescriptor>{e.error()};
  auto f = r.u64(); if (!f.ok()) return Result<StateDescriptor>{f.error()};
  auto g = r.u64(); if (!g.ok()) return Result<StateDescriptor>{g.error()};
  st.id = StateId::from(a.value()); st.generation = b.value(); st.device = DeviceId::from(c.value());
  st.bytes_held = d.value(); st.estimated_growth = e.value(); st.owner_tag = f.value(); st.access_version = g.value();
  return Result<StateDescriptor>::ok(st);
}
static void write_member(Writer& w, const DecodeMemberSpec& m) {
  w.u64(m.sequence.value()); w.u64(m.attempt.value()); w.u64(m.generation.value());
  write_state(w, m.state);
  w.u64(m.current_length); w.u64(m.generated_tokens); w.u64(m.remaining_budget);
  w.bytes(m.payload);
}
static Result<DecodeMemberSpec> read_member(Reader& r) {
  DecodeMemberSpec m;
  auto seq = r.u64(); if (!seq.ok()) return Result<DecodeMemberSpec>{seq.error()};
  auto at = r.u64(); if (!at.ok()) return Result<DecodeMemberSpec>{at.error()};
  auto gen = r.u64(); if (!gen.ok()) return Result<DecodeMemberSpec>{gen.error()};
  auto st = read_state(r); if (!st.ok()) return Result<DecodeMemberSpec>{st.error()};
  auto cl = r.u64(); if (!cl.ok()) return Result<DecodeMemberSpec>{cl.error()};
  auto gt = r.u64(); if (!gt.ok()) return Result<DecodeMemberSpec>{gt.error()};
  auto rb = r.u64(); if (!rb.ok()) return Result<DecodeMemberSpec>{rb.error()};
  auto pl = r.bytes(); if (!pl.ok()) return Result<DecodeMemberSpec>{pl.error()};
  m.sequence = SequenceId::from(seq.value()); m.attempt = AttemptId::from(at.value());
  m.generation = DecodeGeneration::from(gen.value()); m.state = st.value();
  m.current_length = cl.value(); m.generated_tokens = gt.value();
  m.remaining_budget = rb.value(); m.payload = pl.value();
  return Result<DecodeMemberSpec>::ok(m);
}

// ===========================================================================
// ExecuteRequest
// ===========================================================================
std::vector<std::uint8_t> encode_execute_request(const DecodeExecutionRequest& req) {
  Writer w;
  w.u64(req.dispatch_id.value()); w.u64(req.epoch.value());
  w.u64(req.worker.value()); w.u64(req.worker_boot.value());
  w.u64(req.reservation_id);
  write_key(w, req.key);
  write_device(w, req.device);
  w.u64(static_cast<std::uint64_t>(req.members.size()));
  for (const DecodeMemberSpec& m : req.members) write_member(w, m);
  w.bytes(req.group_payload);
  w.ns(req.deadline_hint_ns);
  return w.take();
}
Result<DecodeExecutionRequest> decode_execute_request(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes);
  DecodeExecutionRequest req;
  auto did = r.u64(); if (!did.ok()) return Result<DecodeExecutionRequest>{did.error()};
  auto ep = r.u64(); if (!ep.ok()) return Result<DecodeExecutionRequest>{ep.error()};
  auto wk = r.u64(); if (!wk.ok()) return Result<DecodeExecutionRequest>{wk.error()};
  auto wb = r.u64(); if (!wb.ok()) return Result<DecodeExecutionRequest>{wb.error()};
  auto rid = r.u64(); if (!rid.ok()) return Result<DecodeExecutionRequest>{rid.error()};
  auto key = read_key(r); if (!key.ok()) return Result<DecodeExecutionRequest>{key.error()};
  auto dev = read_device(r); if (!dev.ok()) return Result<DecodeExecutionRequest>{dev.error()};
  auto cnt = r.u64(); if (!cnt.ok()) return Result<DecodeExecutionRequest>{cnt.error()};
  if (cnt.value() > 1u << 18) return Result<DecodeExecutionRequest>{Error{ErrorCode::ProtocolInvalidField, "member count too large"}};
  for (std::uint64_t i = 0; i < cnt.value(); ++i) {
    auto m = read_member(r); if (!m.ok()) return Result<DecodeExecutionRequest>{m.error()};
    req.members.push_back(std::move(m.value()));
  }
  auto gp = r.bytes(); if (!gp.ok()) return Result<DecodeExecutionRequest>{gp.error()};
  auto dh = r.ns(); if (!dh.ok()) return Result<DecodeExecutionRequest>{dh.error()};
  req.dispatch_id = DispatchId::from(did.value()); req.epoch = CoordinatorEpoch::from(ep.value());
  req.worker = WorkerId::from(wk.value()); req.worker_boot = WorkerBootId::from(wb.value());
  req.reservation_id = rid.value(); req.key = key.value(); req.device = dev.value();
  req.group_payload = std::move(gp.value()); req.deadline_hint_ns = dh.value();
  return Result<DecodeExecutionRequest>::ok(std::move(req));
}

// ===========================================================================
// ExecuteResult
// ===========================================================================
std::vector<std::uint8_t> encode_execute_result(const DecodeExecutionResult& res) {
  Writer w;
  w.u64(res.dispatch_id.value()); w.u64(res.epoch.value());
  w.u64(res.worker.value()); w.u64(res.worker_boot.value());
  w.ns(res.group_active_ns);
  w.u32(static_cast<std::uint32_t>(res.group_error));
  w.str(res.group_error_message);
  w.u64(static_cast<std::uint64_t>(res.outcomes.size()));
  for (const MemberOutcome& mo : res.outcomes) {
    w.u64(mo.sequence.value());
    w.u8(static_cast<std::uint8_t>(mo.kind));
    w.u64(mo.generated); w.u64(mo.token_identifier);
    w.u8(mo.terminal ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(mo.error_code)); w.str(mo.error_message);
    w.u8(mo.retryable ? 1 : 0);
    w.ns(mo.started_at.ns); w.ns(mo.finished_at.ns); w.ns(mo.active_ns);
    w.u64(mo.kv_bytes_delta); w.u64(mo.kv_bytes_after);
    w.u64(mo.attempt.value()); w.u64(mo.generation.value());
  }
  return w.take();
}
Result<DecodeExecutionResult> decode_execute_result(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes);
  DecodeExecutionResult res;
  auto did = r.u64(); if (!did.ok()) return Result<DecodeExecutionResult>{did.error()};
  auto ep = r.u64(); if (!ep.ok()) return Result<DecodeExecutionResult>{ep.error()};
  auto wk = r.u64(); if (!wk.ok()) return Result<DecodeExecutionResult>{wk.error()};
  auto wb = r.u64(); if (!wb.ok()) return Result<DecodeExecutionResult>{wb.error()};
  auto ga = r.ns(); if (!ga.ok()) return Result<DecodeExecutionResult>{ga.error()};
  auto ge = r.u32(); if (!ge.ok()) return Result<DecodeExecutionResult>{ge.error()};
  auto gem = r.str(); if (!gem.ok()) return Result<DecodeExecutionResult>{gem.error()};
  auto cnt = r.u64(); if (!cnt.ok()) return Result<DecodeExecutionResult>{cnt.error()};
  if (cnt.value() > 1u << 18) return Result<DecodeExecutionResult>{Error{ErrorCode::ProtocolInvalidField, "outcome count too large"}};
  for (std::uint64_t i = 0; i < cnt.value(); ++i) {
    MemberOutcome mo;
    auto seq = r.u64(); if (!seq.ok()) return Result<DecodeExecutionResult>{seq.error()};
    auto k = r.u8(); if (!k.ok()) return Result<DecodeExecutionResult>{k.error()};
    auto gn = r.u64(); if (!gn.ok()) return Result<DecodeExecutionResult>{gn.error()};
    auto tid = r.u64(); if (!tid.ok()) return Result<DecodeExecutionResult>{tid.error()};
    auto te = r.u8(); if (!te.ok()) return Result<DecodeExecutionResult>{te.error()};
    auto ec = r.u32(); if (!ec.ok()) return Result<DecodeExecutionResult>{ec.error()};
    auto em = r.str(); if (!em.ok()) return Result<DecodeExecutionResult>{em.error()};
    auto rb = r.u8(); if (!rb.ok()) return Result<DecodeExecutionResult>{rb.error()};
    auto sa = r.ns(); if (!sa.ok()) return Result<DecodeExecutionResult>{sa.error()};
    auto fa = r.ns(); if (!fa.ok()) return Result<DecodeExecutionResult>{fa.error()};
    auto an = r.ns(); if (!an.ok()) return Result<DecodeExecutionResult>{an.error()};
    auto kd = r.u64(); if (!kd.ok()) return Result<DecodeExecutionResult>{kd.error()};
    auto ka = r.u64(); if (!ka.ok()) return Result<DecodeExecutionResult>{ka.error()};
    auto at = r.u64(); if (!at.ok()) return Result<DecodeExecutionResult>{at.error()};
    auto gr = r.u64(); if (!gr.ok()) return Result<DecodeExecutionResult>{gr.error()};
    mo.sequence = SequenceId::from(seq.value());
    mo.kind = static_cast<MemberOutcomeKind>(k.value());
    mo.generated = gn.value(); mo.token_identifier = tid.value();
    mo.terminal = te.value() != 0;
    mo.error_code = static_cast<ErrorCode>(ec.value()); mo.error_message = em.value();
    mo.retryable = rb.value() != 0;
    mo.started_at = TimePoint(sa.value()); mo.finished_at = TimePoint(fa.value());
    mo.active_ns = an.value(); mo.kv_bytes_delta = kd.value(); mo.kv_bytes_after = ka.value();
    mo.attempt = AttemptId::from(at.value()); mo.generation = DecodeGeneration::from(gr.value());
    res.outcomes.push_back(std::move(mo));
  }
  res.dispatch_id = DispatchId::from(did.value()); res.epoch = CoordinatorEpoch::from(ep.value());
  res.worker = WorkerId::from(wk.value()); res.worker_boot = WorkerBootId::from(wb.value());
  res.group_active_ns = ga.value(); res.group_error = static_cast<ErrorCode>(ge.value());
  res.group_error_message = gem.value();
  return Result<DecodeExecutionResult>::ok(std::move(res));
}

// ===========================================================================
// SubmitRequest / StatusReply / SnapshotReply / Query
// ===========================================================================
std::vector<std::uint8_t> encode_submit_request(const DecodeRequest& req) {
  Writer w;
  w.u64(req.id.value()); w.u64(req.initial_attempt.value()); w.u64(req.tenant.value());
  w.u64(req.model.value()); w.u64(req.revision.value()); w.u64(req.adapter.value());
  w.u64(req.sequence.value());
  w.u64(req.prompt_length); w.u64(req.max_generation_length); w.u64(req.pregenerated);
  w.u32(req.priority);
  std::uint64_t wbits; std::memcpy(&wbits, &req.tenant_weight, sizeof(wbits)); w.u64(wbits);
  w.u8(static_cast<std::uint8_t>(req.latency_class));
  w.ns(req.deadline.ns); w.ns(req.per_token_target_ns);
  w.u64(req.estimated_kv_growth_per_token); w.u64(req.device_constraint.value());
  w.str(req.sampling_metadata);
  write_state(w, req.state);
  w.ns(req.arrival.ns);
  return w.take();
}
Result<DecodeRequest> decode_submit_request(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes);
  DecodeRequest req;
  auto id = r.u64(); if (!id.ok()) return Result<DecodeRequest>{id.error()};
  auto ia = r.u64(); if (!ia.ok()) return Result<DecodeRequest>{ia.error()};
  auto tn = r.u64(); if (!tn.ok()) return Result<DecodeRequest>{tn.error()};
  auto md = r.u64(); if (!md.ok()) return Result<DecodeRequest>{md.error()};
  auto rv = r.u64(); if (!rv.ok()) return Result<DecodeRequest>{rv.error()};
  auto ad = r.u64(); if (!ad.ok()) return Result<DecodeRequest>{ad.error()};
  auto sq = r.u64(); if (!sq.ok()) return Result<DecodeRequest>{sq.error()};
  auto pl = r.u64(); if (!pl.ok()) return Result<DecodeRequest>{pl.error()};
  auto mg = r.u64(); if (!mg.ok()) return Result<DecodeRequest>{mg.error()};
  auto pg = r.u64(); if (!pg.ok()) return Result<DecodeRequest>{pg.error()};
  auto pr = r.u32(); if (!pr.ok()) return Result<DecodeRequest>{pr.error()};
  auto wb = r.u64(); if (!wb.ok()) return Result<DecodeRequest>{wb.error()};
  auto lc = r.u8(); if (!lc.ok()) return Result<DecodeRequest>{lc.error()};
  auto dl = r.ns(); if (!dl.ok()) return Result<DecodeRequest>{dl.error()};
  auto pt = r.ns(); if (!pt.ok()) return Result<DecodeRequest>{pt.error()};
  auto kg = r.u64(); if (!kg.ok()) return Result<DecodeRequest>{kg.error()};
  auto dc = r.u64(); if (!dc.ok()) return Result<DecodeRequest>{dc.error()};
  auto sm = r.str(); if (!sm.ok()) return Result<DecodeRequest>{sm.error()};
  auto st = read_state(r); if (!st.ok()) return Result<DecodeRequest>{st.error()};
  auto ar = r.ns(); if (!ar.ok()) return Result<DecodeRequest>{ar.error()};
  req.id = RequestId::from(id.value()); req.initial_attempt = AttemptId::from(ia.value());
  req.tenant = TenantId::from(tn.value()); req.model = ModelId::from(md.value());
  req.revision = ModelRevision::from(rv.value()); req.adapter = AdapterId::from(ad.value());
  req.sequence = SequenceId::from(sq.value());
  req.prompt_length = pl.value(); req.max_generation_length = mg.value(); req.pregenerated = pg.value();
  req.priority = pr.value();
  double wv; std::uint64_t wbits = wb.value(); std::memcpy(&wv, &wbits, sizeof(wv)); req.tenant_weight = wv;
  req.latency_class = static_cast<LatencyClass>(lc.value()); req.deadline = TimePoint(dl.value());
  req.per_token_target_ns = pt.value(); req.estimated_kv_growth_per_token = kg.value();
  req.device_constraint = DeviceId::from(dc.value()); req.sampling_metadata = std::move(sm.value());
  req.state = st.value(); req.arrival = TimePoint(ar.value());
  return Result<DecodeRequest>::ok(std::move(req));
}

std::vector<std::uint8_t> encode_submit_ack(const SubmitAck& a) {
  Writer w; w.u8(a.admitted ? 1 : 0); w.u64(a.sequence.value()); w.str(a.reason); return w.take();
}
Result<SubmitAck> decode_submit_ack(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); SubmitAck a;
  auto ad = r.u8(); if (!ad.ok()) return Result<SubmitAck>{ad.error()};
  auto sq = r.u64(); if (!sq.ok()) return Result<SubmitAck>{sq.error()};
  auto rs = r.str(); if (!rs.ok()) return Result<SubmitAck>{rs.error()};
  a.admitted = ad.value() != 0; a.sequence = SequenceId::from(sq.value()); a.reason = rs.value();
  return Result<SubmitAck>::ok(a);
}
std::vector<std::uint8_t> encode_status_reply(const StatusReply& s) {
  Writer w; w.u32(s.active); w.u32(s.ready); w.u64(s.generated_tokens);
  w.u64(s.decode_steps); w.u32(s.groups); w.u64(s.requests_admitted);
  w.u64(s.stale_rejections); w.u64(s.epoch); return w.take();
}
Result<StatusReply> decode_status_reply(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); StatusReply s;
  auto a = r.u32(); if (!a.ok()) return Result<StatusReply>{a.error()};
  auto b = r.u32(); if (!b.ok()) return Result<StatusReply>{b.error()};
  auto c = r.u64(); if (!c.ok()) return Result<StatusReply>{c.error()};
  auto d = r.u64(); if (!d.ok()) return Result<StatusReply>{d.error()};
  auto e = r.u32(); if (!e.ok()) return Result<StatusReply>{e.error()};
  auto f = r.u64(); if (!f.ok()) return Result<StatusReply>{f.error()};
  auto g = r.u64(); if (!g.ok()) return Result<StatusReply>{g.error()};
  auto h = r.u64(); if (!h.ok()) return Result<StatusReply>{h.error()};
  s.active = a.value(); s.ready = b.value(); s.generated_tokens = c.value();
  s.decode_steps = d.value(); s.groups = e.value(); s.requests_admitted = f.value();
  s.stale_rejections = g.value(); s.epoch = h.value();
  return Result<StatusReply>::ok(s);
}
std::vector<std::uint8_t> encode_snapshot_reply(const std::string& json) {
  Writer w; w.str(json); return w.take();
}
Result<std::string> decode_snapshot_reply(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); auto s = r.str(); if (!s.ok()) return Result<std::string>{s.error()}; return Result<std::string>::ok(s.value());
}
std::vector<std::uint8_t> encode_query(const QueryRequest& q) {
  Writer w; w.u64(q.request.value()); w.u64(q.sequence.value()); w.u8(q.want_snapshot ? 1 : 0); return w.take();
}
Result<QueryRequest> decode_query(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); QueryRequest q;
  auto a = r.u64(); if (!a.ok()) return Result<QueryRequest>{a.error()};
  auto b = r.u64(); if (!b.ok()) return Result<QueryRequest>{b.error()};
  auto c = r.u8(); if (!c.ok()) return Result<QueryRequest>{c.error()};
  q.request = RequestId::from(a.value()); q.sequence = SequenceId::from(b.value()); q.want_snapshot = c.value() != 0;
  return Result<QueryRequest>::ok(q);
}
std::vector<std::uint8_t> encode_explain_request(const ExplainRequest& q) {
  Writer w; w.u64(q.sequence.value()); w.str(q.question); return w.take();
}
Result<ExplainRequest> decode_explain_request(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); ExplainRequest q;
  auto a = r.u64(); if (!a.ok()) return Result<ExplainRequest>{a.error()};
  auto b = r.str(); if (!b.ok()) return Result<ExplainRequest>{b.error()};
  q.sequence = SequenceId::from(a.value()); q.question = b.value();
  return Result<ExplainRequest>::ok(q);
}
std::vector<std::uint8_t> encode_explain_reply(const ExplainReply& q) {
  Writer w; w.str(q.text); w.str(q.json); return w.take();
}
Result<ExplainReply> decode_explain_reply(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); ExplainReply q;
  auto a = r.str(); if (!a.ok()) return Result<ExplainReply>{a.error()};
  auto b = r.str(); if (!b.ok()) return Result<ExplainReply>{b.error()};
  q.text = a.value(); q.json = b.value();
  return Result<ExplainReply>::ok(q);
}
std::vector<std::uint8_t> encode_get_authority(const GetAuthority& g) {
  Writer w; w.u64(g.sequence.value()); return w.take();
}
Result<GetAuthority> decode_get_authority(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); GetAuthority g;
  auto a = r.u64(); if (!a.ok()) return Result<GetAuthority>{a.error()};
  g.sequence = SequenceId::from(a.value());
  return Result<GetAuthority>::ok(g);
}
std::vector<std::uint8_t> encode_authority_reply(const AuthorityReply& a) {
  Writer w; w.u8(a.exists ? 1 : 0); w.u64(a.sequence.value()); w.u64(a.dispatch.value());
  w.u64(a.epoch.value()); w.u64(a.worker.value()); w.u64(a.worker_boot.value());
  w.u64(a.attempt.value()); w.u64(a.generation.value()); w.u64(a.generated); return w.take();
}
Result<AuthorityReply> decode_authority_reply(const std::vector<std::uint8_t>& bytes) {
  Reader r(bytes); AuthorityReply a;
  auto e = r.u8(); if (!e.ok()) return Result<AuthorityReply>{e.error()};
  auto s = r.u64(); if (!s.ok()) return Result<AuthorityReply>{s.error()};
  auto d = r.u64(); if (!d.ok()) return Result<AuthorityReply>{d.error()};
  auto p = r.u64(); if (!p.ok()) return Result<AuthorityReply>{p.error()};
  auto wk = r.u64(); if (!wk.ok()) return Result<AuthorityReply>{wk.error()};
  auto wb = r.u64(); if (!wb.ok()) return Result<AuthorityReply>{wb.error()};
  auto at0 = r.u64(); if (!at0.ok()) return Result<AuthorityReply>{at0.error()};
  auto g = r.u64(); if (!g.ok()) return Result<AuthorityReply>{g.error()};
  auto gn = r.u64(); if (!gn.ok()) return Result<AuthorityReply>{gn.error()};
  a.exists = e.value() != 0; a.sequence = SequenceId::from(s.value()); a.dispatch = DispatchId::from(d.value());
  a.epoch = CoordinatorEpoch::from(p.value()); a.worker = WorkerId::from(wk.value()); a.worker_boot = WorkerBootId::from(wb.value());
  a.attempt = AttemptId::from(at0.value()); a.generation = DecodeGeneration::from(g.value()); a.generated = gn.value();
  return Result<AuthorityReply>::ok(a);
}

}  // namespace decodefabric::protocol
