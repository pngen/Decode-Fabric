#pragma once
#include <cstdint>
#include "decodefabric/error.hpp"

namespace decodefabric {

// Generated-token counts and budgets. All arithmetic is overflow-safe: no
// counter may wrap on a legitimate value, and exhaustion is deterministic and
// terminal.
using TokenCount = std::uint64_t;

namespace detail {
// Saturating checked addition: on overflow the value is clamped and reported.
inline Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
  if (b > UINT64_MAX - a) {
    return Result<std::uint64_t>{Error{ErrorCode::Overflow, "token counter overflow"}};
  }
  return Result<std::uint64_t>::ok(a + b);
}
}  // namespace detail

// A generation budget: how many tokens a sequence may generate before it must
// terminate. Remaining is always max - generated (never negative).
class TokenBudget {
 public:
  TokenBudget() = default;
  explicit TokenBudget(std::uint64_t max_generated) : max_(max_generated) {}

  std::uint64_t max() const noexcept { return max_; }
  std::uint64_t generated() const noexcept { return generated_; }
  std::uint64_t remaining() const noexcept {
    return generated_ >= max_ ? 0 : max_ - generated_;
  }
  bool exhausted() const noexcept { return generated_ >= max_; }
  bool valid() const noexcept { return max_ > 0; }

  // Advance by one authoritatively generated token. Returns an error on
  // overflow or when the budget is already exhausted.
  Result<void> advance(std::uint64_t by = 1) {
    auto r = detail::checked_add(generated_, by);
    if (!r.ok()) return Result<void>{r.error()};
    std::uint64_t next = r.value();
    if (next > max_) {
      return Result<void>{Error{ErrorCode::Backpressure,
                                "generation budget exhausted"}};
    }
    generated_ = next;
    return Result<void>::success();
  }

 private:
  std::uint64_t max_ = 0;
  std::uint64_t generated_ = 0;
};

}  // namespace decodefabric
