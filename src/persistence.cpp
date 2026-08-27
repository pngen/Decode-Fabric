#include "decodefabric/persistence.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace decodefabric {

namespace fs = std::filesystem;

// Turn an arbitrary scope key into a safe, deterministic filename component.
static std::string sanitize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
      out.push_back(c);
    else
      out.push_back('_');
  }
  return out;
}

static std::string record_path(const std::string& dir, const std::string& scope) {
  return (fs::path(dir) / (sanitize(scope) + ".rec")).string();
}

FilePersistence::FilePersistence(std::string directory) : directory_(std::move(directory)) {
  std::error_code ec;
  fs::create_directories(directory_, ec);
}

Result<void> FilePersistence::write(const std::string& scope,
                                    const std::vector<std::uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(directory_, ec);
  std::string path = record_path(directory_, scope);
  std::string tmp = path + ".tmp";
  {
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs) return failed<void>(ErrorCode::PersistenceIoError, "cannot open write temp");
    if (!bytes.empty()) ofs.write(reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<std::streamsize>(bytes.size()));
    if (!ofs) return failed<void>(ErrorCode::PersistenceIoError, "cannot write record");
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
  }
  if (ec) return failed<void>(ErrorCode::PersistenceIoError, "cannot commit record");
  return Result<void>::success();
}

Result<std::vector<std::uint8_t>> FilePersistence::read(const std::string& scope) {
  std::string path = record_path(directory_, scope);
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) return failed<std::vector<std::uint8_t>>(ErrorCode::PersistenceIoError, "record not found");
  std::streamsize n = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n));
  if (n > 0) ifs.read(reinterpret_cast<char*>(bytes.data()), n);
  return Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

Result<bool> FilePersistence::exists(const std::string& scope) {
  std::error_code ec;
  bool r = fs::exists(record_path(directory_, scope), ec);
  return Result<bool>::ok(r);
}

Result<void> FilePersistence::remove(const std::string& scope) {
  std::error_code ec;
  fs::remove(record_path(directory_, scope), ec);
  return Result<void>::success();
}

Result<std::vector<std::string>> FilePersistence::list() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(directory_, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".rec") {
      out.push_back(entry.path().stem().string());
    }
  }
  return Result<std::vector<std::string>>::ok(std::move(out));
}

}  // namespace decodefabric
