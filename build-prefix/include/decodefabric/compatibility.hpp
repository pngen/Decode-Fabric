#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/backend.hpp"
#include "decodefabric/ids.hpp"

namespace decodefabric {

// A typed, deterministic CompatibilityKey. Two sequences share an identical
// key only when every element necessary to run them together within one decode
// group is equal: model + revision, adapter relationship, executor/backend,
// device, numerical dtype, tensor/layout compatibility, KV representation
// compatibility, sequence constraints required by the executor, and operator
// policy. Decode Fabric uses this key to form execution groups; sequences with
// different keys cannot be packed together.
struct CompatibilityKey {
  ModelId model;
  ModelRevision revision;
  AdapterId adapter;            // invalid / null when no adapter is in play
  BackendKind backend = BackendKind::CPU;
  DeviceId device;
  DType dtype = DType::F32;
  std::uint32_t tensor_layout = 0;          // tensor/layout compatibility tag
  std::uint32_t kv_representation = 0;      // KV state representation tag
  std::uint32_t sequence_requirements = 0;  // executor-required sequence constraints
  std::uint32_t operator_policy = 0;        // operator policy tag

  bool valid() const noexcept { return model.is_valid() && device.is_valid(); }

  friend bool operator==(const CompatibilityKey&, const CompatibilityKey&) = default;

  bool operator<(const CompatibilityKey& o) const noexcept;

  // Canonical, deterministic textual form (stable across runs and platforms).
  std::string to_string() const;

  // Deterministic 64-bit hash over the canonical bytes.
  std::uint64_t hash() const noexcept;
};

struct CompatibilityKeyHash {
  std::size_t operator()(const CompatibilityKey& k) const noexcept {
    return static_cast<std::size_t>(k.hash());
  }
};

// An explainable compatibility decision. It records whether the key is
// compatible and, when it is not, which components differ and why.
struct CompatibilityDecision {
  bool compatible = false;
  CompatibilityKey key;                 // the resolved key (may be invalid/zero)
  std::string reason;                   // human-readable explanation
  std::vector<std::string> matched;     // components that matched
  std::vector<std::string> mismatched;  // components that differed

  std::string to_string() const;
  std::string to_json() const;
};

// Evaluate compatibility of two keys, producing an explainable decision.
CompatibilityDecision evaluate_compatibility(const CompatibilityKey& a,
                                             const CompatibilityKey& b);

}  // namespace decodefabric
