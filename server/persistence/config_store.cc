#include "server/persistence/config_store.h"

#include <sqlite3.h>

#include "common/sha256/sha256.h"

namespace talos::server {

namespace {

std::string hex(std::string_view data) {
  const auto d = talos::sha256(data);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char b : d) {
    out += kHex[b >> 4];
    out += kHex[b & 0xF];
  }
  return out;
}

}  // namespace

Result<std::unique_ptr<SqliteConfigStore>> SqliteConfigStore::open(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    std::string msg = db ? sqlite3_errmsg(db) : "open failed";
    sqlite3_close(db);
    return Error{"storage_error", msg};
  }
  sqlite3_busy_timeout(db, 5000);
  char* err = nullptr;
  if (sqlite3_exec(db,
                   "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                   "CREATE TABLE IF NOT EXISTS config_revisions ("
                   "  revision INTEGER PRIMARY KEY,"
                   "  created_at_ms INTEGER NOT NULL,"
                   "  principal TEXT NOT NULL,"
                   "  sha256 TEXT NOT NULL,"
                   "  document BLOB NOT NULL);",
                   nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "init failed";
    sqlite3_free(err);
    sqlite3_close(db);
    return Error{"storage_error", msg};
  }
  return std::unique_ptr<SqliteConfigStore>(new SqliteConfigStore(db));
}

SqliteConfigStore::~SqliteConfigStore() { sqlite3_close(db_); }

std::optional<ConfigRevision> SqliteConfigStore::query(const char* sql, std::int64_t arg,
                                                       bool bind) const {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) return std::nullopt;
  if (bind) sqlite3_bind_int64(s, 1, arg);
  std::optional<ConfigRevision> out;
  if (sqlite3_step(s) == SQLITE_ROW) {
    ConfigRevision r;
    r.revision = static_cast<std::uint32_t>(sqlite3_column_int64(s, 0));
    r.created_at = Timestamp{sqlite3_column_int64(s, 1)};
    r.principal = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
    r.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(s, 3));
    if (r.document.ParseFromArray(sqlite3_column_blob(s, 4), sqlite3_column_bytes(s, 4))) {
      out = std::move(r);
    }
  }
  sqlite3_finalize(s);
  return out;
}

std::optional<ConfigRevision> SqliteConfigStore::latest() const {
  return query(
      "SELECT revision, created_at_ms, principal, sha256, document FROM config_revisions "
      "ORDER BY revision DESC LIMIT 1;",
      0, false);
}

std::optional<ConfigRevision> SqliteConfigStore::get(std::uint32_t revision) const {
  return query(
      "SELECT revision, created_at_ms, principal, sha256, document FROM config_revisions "
      "WHERE revision = ?;",
      revision, true);
}

Result<ConfigRevision> SqliteConfigStore::append(const talos::server::v1::ConfigDocument& doc,
                                                 const std::string& principal,
                                                 Timestamp created_at) {
  std::string bytes;
  if (!doc.SerializeToString(&bytes)) return Error{"encode_error", "config document"};
  ConfigRevision r;
  r.revision = latest() ? latest()->revision + 1 : 1;
  r.created_at = created_at;
  r.principal = principal;
  r.sha256 = hex(bytes);
  r.document = doc;
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT INTO config_revisions(revision, created_at_ms, principal, sha256, "
                         "document) VALUES (?, ?, ?, ?, ?);",
                         -1, &s, nullptr) != SQLITE_OK) {
    return Error{"storage_error", sqlite3_errmsg(db_)};
  }
  sqlite3_bind_int64(s, 1, r.revision);
  sqlite3_bind_int64(s, 2, created_at.unix_ms);
  sqlite3_bind_text(s, 3, principal.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 4, r.sha256.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(s, 5, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
  const int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE) return Error{"storage_error", sqlite3_errmsg(db_)};
  return r;
}

}  // namespace talos::server
