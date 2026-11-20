// Verified firmware releases (manifest + image) in the hub's SQLite file.
//
// Releases are keyed by (board_id, board_revision, version): two boards can
// ship the same build number, and an existing release is never replaced
// (red team #5). An empty board_revision means "any revision of the board".
#pragma once

#include <cstdint>
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

// Strict build number: 1-10 ASCII digits, no sign, > 0 and <= UINT32_MAX.
// Never wraps.
Result<std::uint32_t> parse_release_version(const std::string& version);

class SqliteFirmwareStore {
 public:
  static Result<std::unique_ptr<SqliteFirmwareStore>> open(const std::string& path);
  ~SqliteFirmwareStore();
  SqliteFirmwareStore(const SqliteFirmwareStore&) = delete;
  SqliteFirmwareStore& operator=(const SqliteFirmwareStore&) = delete;

  // Fails with `conflict` if a release with the same key already exists.
  Status put(const talos::v1::FirmwareRelease& manifest, const std::string& image);
  // Exact key lookup.
  std::optional<StoredRelease> get(const std::string& board_id, const std::string& board_revision,
                                   std::uint32_t version) const;
  // The release a device on (board, revision) would take at `version`: an
  // exact revision match first, then a revision-agnostic release.
  std::optional<StoredRelease> find_for(const std::string& board_id,
                                        const std::string& board_revision,
                                        std::uint32_t version) const;
  std::vector<talos::v1::FirmwareRelease> list() const;

 private:
  explicit SqliteFirmwareStore(sqlite3* db) : db_(db) {}
  sqlite3* db_;
};

}  // namespace talos::server
