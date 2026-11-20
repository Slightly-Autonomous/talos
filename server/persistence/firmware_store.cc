#include "server/persistence/firmware_store.h"

#include <sqlite3.h>

namespace talos::server {

Result<std::uint32_t> parse_release_version(const std::string& v) {
  auto bad = [] {
    return Error{"invalid_release", "version must be a build number from 1 to 4294967295"};
  };
  if (v.empty() || v.size() > 10) return bad();
  std::uint64_t out = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return bad();
    out = out * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (out == 0 || out > 0xFFFF'FFFFull) return bad();
  return static_cast<std::uint32_t>(out);
}

namespace {

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS firmware_releases_v2 ("
    "  board_id TEXT NOT NULL, board_revision TEXT NOT NULL, version INTEGER NOT NULL,"
    "  manifest BLOB NOT NULL, image BLOB NOT NULL,"
    "  PRIMARY KEY (board_id, board_revision, version));";

bool insert(sqlite3* db, const talos::v1::FirmwareRelease& m, std::uint32_t version,
            const void* image, int image_len) {
  std::string bytes;
  if (!m.SerializeToString(&bytes)) return false;
  sqlite3_stmt* s = nullptr;
  // Plain INSERT: an existing key is a conflict, never a silent replace.
  sqlite3_prepare_v2(db, "INSERT INTO firmware_releases_v2 VALUES (?, ?, ?, ?, ?);", -1, &s,
                     nullptr);
  sqlite3_bind_text(s, 1, m.board_id().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, m.board_revision().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(s, 3, version);
  sqlite3_bind_blob(s, 4, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(s, 5, image, image_len, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  return rc == SQLITE_DONE;
}

// The v1 table was keyed by version alone. Move its rows over (first wins on
// a key collision) and drop it.
void migrate_v1(sqlite3* db) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT manifest, image FROM firmware_releases;", -1, &s, nullptr) !=
      SQLITE_OK) {
    sqlite3_finalize(s);
    return;  // no v1 table
  }
  while (sqlite3_step(s) == SQLITE_ROW) {
    talos::v1::FirmwareRelease m;
    if (!m.ParseFromArray(sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0))) continue;
    auto version = parse_release_version(m.version());
    if (!version) continue;
    (void)insert(db, m, version.value(), sqlite3_column_blob(s, 1), sqlite3_column_bytes(s, 1));
  }
  sqlite3_finalize(s);
  sqlite3_exec(db, "DROP TABLE firmware_releases;", nullptr, nullptr, nullptr);
}

}  // namespace

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
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
    std::string msg = sqlite3_errmsg(db);
    sqlite3_close(db);
    return Error{"storage_error", msg};
  }
  migrate_v1(db);
  return std::unique_ptr<SqliteFirmwareStore>(new SqliteFirmwareStore(db));
}

SqliteFirmwareStore::~SqliteFirmwareStore() { sqlite3_close(db_); }

Status SqliteFirmwareStore::put(const talos::v1::FirmwareRelease& m, const std::string& image) {
  auto version = parse_release_version(m.version());
  if (!version) return version.error();
  if (m.board_id().empty()) return Error{"invalid_release", "board_id is required"};
  if (get(m.board_id(), m.board_revision(), version.value())) {
    return Error{"conflict", "release " + m.board_id() + "/" + m.board_revision() + " v" +
                                 m.version() + " already exists; releases are never replaced"};
  }
  if (!insert(db_, m, version.value(), image.data(), static_cast<int>(image.size()))) {
    return Error{"storage_error", sqlite3_errmsg(db_)};
  }
  return Status::Ok();
}

std::optional<StoredRelease> SqliteFirmwareStore::get(const std::string& board_id,
                                                      const std::string& board_revision,
                                                      std::uint32_t version) const {
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db_,
                     "SELECT manifest, image FROM firmware_releases_v2 "
                     "WHERE board_id = ? AND board_revision = ? AND version = ?;",
                     -1, &s, nullptr);
  sqlite3_bind_text(s, 1, board_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, board_revision.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(s, 3, version);
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

std::optional<StoredRelease> SqliteFirmwareStore::find_for(const std::string& board_id,
                                                           const std::string& board_revision,
                                                           std::uint32_t version) const {
  if (!board_revision.empty()) {
    if (auto exact = get(board_id, board_revision, version)) return exact;
  }
  return get(board_id, "", version);
}

std::vector<talos::v1::FirmwareRelease> SqliteFirmwareStore::list() const {
  std::vector<talos::v1::FirmwareRelease> out;
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(
      db_, "SELECT manifest FROM firmware_releases_v2 ORDER BY board_id, board_revision, version;",
      -1, &s, nullptr);
  while (sqlite3_step(s) == SQLITE_ROW) {
    talos::v1::FirmwareRelease m;
    (void)m.ParseFromArray(sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));
    out.push_back(std::move(m));
  }
  sqlite3_finalize(s);
  return out;
}

}  // namespace talos::server
