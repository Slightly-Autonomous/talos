// Verified firmware releases (manifest + image) in the hub's SQLite file.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/base/result.h"
#include "talos/v1/firmware.pb.h"

struct sqlite3;

namespace talos::server {

struct StoredRelease {
  talos::v1::FirmwareRelease manifest;
  std::string image;
};

class SqliteFirmwareStore {
 public:
  static Result<std::unique_ptr<SqliteFirmwareStore>> open(const std::string& path);
  ~SqliteFirmwareStore();
  SqliteFirmwareStore(const SqliteFirmwareStore&) = delete;
  SqliteFirmwareStore& operator=(const SqliteFirmwareStore&) = delete;

  Status put(const talos::v1::FirmwareRelease& manifest, const std::string& image);
  std::optional<StoredRelease> get(std::uint32_t version) const;
  std::vector<talos::v1::FirmwareRelease> list() const;

 private:
  explicit SqliteFirmwareStore(sqlite3* db) : db_(db) {}
  sqlite3* db_;
};

}  // namespace talos::server
