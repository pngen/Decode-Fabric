#pragma once
#include <cstdint>

namespace decodefabric {

// Concrete accelerator/backend kinds Decode Fabric can take as an executor
// backend. CPU is required and always present; CUDA is one concrete backend and
// is never an assumption embedded through the whole type system.
enum class BackendKind : std::uint8_t {
  CPU = 0,
  CUDA = 1,
};

// Numerical mode / dtype used by an executor.
enum class DType : std::uint8_t {
  Invalid = 0,
  F32 = 1,
  F16 = 2,
  BF16 = 3,
  F64 = 4,
  I32 = 5,
  I64 = 6,
  U8 = 7,
};

const char* to_string(BackendKind kind) noexcept;
const char* to_string(DType dtype) noexcept;

}  // namespace decodefabric
