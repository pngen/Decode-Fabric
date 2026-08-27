#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "decodefabric/error.hpp"

namespace decodefabric {

class Persistence {
 public:
  virtual ~Persistence() = default;
  virtual Result<void> write(const std::string& scope,
                             const std::vector<std::uint8_t>& bytes) = 0;
  virtual Result<std::vector<std::uint8_t>> read(const std::string& scope) = 0;
  virtual Result<bool> exists(const std::string& scope) = 0;
  virtual Result<void> remove(const std::string& scope) = 0;
  virtual Result<std::vector<std::string>> list() = 0;
};

class FilePersistence final : public Persistence {
 public:
  explicit FilePersistence(std::string directory);
  Result<void> write(const std::string& scope,
                     const std::vector<std::uint8_t>& bytes) override;
  Result<std::vector<std::uint8_t>> read(const std::string& scope) override;
  Result<bool> exists(const std::string& scope) override;
  Result<void> remove(const std::string& scope) override;
  Result<std::vector<std::string>> list() override;

 private:
  std::string directory_;
};

}  // namespace decodefabric
