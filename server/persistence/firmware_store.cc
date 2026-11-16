#include "server/persistence/firmware_store.h"

#include <sqlite3.h>

namespace talos::server {

Result<std::unique_ptr<SqliteFirmwareStore>> SqliteFirmwareStore::open(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    std::string msg = db ? sqlite3_errmsg(db) : "open failed";
    sqlite3_close(db);
    return Error{"storage_error", msg};
  }
  sqlite3_busy_timeout(db, 5000);
  if (sqlite3_exec(db,
                   "CREATE TABLE IF NOT EXISTS firmware_releases ("
                   "  version INTEGER PRIMARY KEY, manifest BLOB NOT NULL, image BLOB NOT NULL);",
                   nullptr, nullptr, nullptr) != SQLITE_OK) {
    std::string msg = sqlite3_errmsg(db);
    sqlite3_close(db);
    return Error{"storage_error", msg};
  }
  return std::unique_ptr<SqliteFirmwareStore>(new SqliteFirmwareStore(db));
}

SqliteFirmwareStore::~SqliteFirmwareStore() { sqlite3_close(db_); }

Status SqliteFirmwareStore::put(const talos::v1::FirmwareRelease& m, const std::string& image) {
  std::uint32_t version = 0;
  for (char c : m.version()) {
    if (c < '0' || c > '9')
      return Error{"invalid_release", "version must be an integer build number"};
    version = version * 10 + static_cast<std::uint32_t>(c - '0');
  }
  std::string bytes;
  if (!m.SerializeToString(&bytes)) return Error{"encode_error", "manifest"};
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO firmware_releases VALUES (?, ?, ?);", -1, &s,
                     nullptr);
  sqlite3_bind_int64(s, 1, version);
  sqlite3_bind_blob(s, 2, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(s, 3, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
  const int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE) return Error{"storage_error", sqlite3_errmsg(db_)};
  return Status::Ok();
}

std::optional<StoredRelease> SqliteFirmwareStore::get(std::uint32_t version) const {
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT manifest, image FROM firmware_releases WHERE version = ?;", -1,
                     &s, nullptr);
  sqlite3_bind_int64(s, 1, version);
  std::optional<StoredRelease> out;
  if (sqlite3_step(s) == SQLITE_ROW) {
    StoredRelease r;
    (void)r.manifest.ParseFromArray(sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
    r.image.assign(static_cast<const char*>(sqlite3_column_blob(s, 1)),
                   static_cast<std::size_t>(sqlite3_column_bytes(s, 1)));
    out = std::move(r);
  }
  sqlite3_finalize(s);
  return out;
}

std::vector<talos::v1::FirmwareRelease> SqliteFirmwareStore::list() const {
  std::vector<talos::v1::FirmwareRelease> out;
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_, "SELECT manifest FROM firmware_releases ORDER BY version;", -1, &s,
                     nullptr);
  while (sqlite3_step(s) == SQLITE_ROW) {
    talos::v1::FirmwareRelease m;
    (void)m.ParseFromArray(sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
    out.push_back(std::move(m));
  }
  sqlite3_finalize(s);
  return out;
}

}  // namespace talos::server
