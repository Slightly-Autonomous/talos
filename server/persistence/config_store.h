// Versioned configuration documents in the hub's SQLite file (ADR 0012).
// Revisions are append-only; the journal records when each one applied.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/base/result.h"
#include "core/time/time.h"
#include "talos/server/v1/config.pb.h"

struct sqlite3;

namespace talos::server {

struct ConfigRevision {
  std::uint32_t revision = 0;
  Timestamp created_at;
  std::string principal;
  std::string sha256;
  talos::server::v1::ConfigDocument document;
};

class SqliteConfigStore {
 public:
  static Result<std::unique_ptr<SqliteConfigStore>> open(const std::string& path);
  ~SqliteConfigStore();
  SqliteConfigStore(const SqliteConfigStore&) = delete;
  SqliteConfigStore& operator=(const SqliteConfigStore&) = delete;

  std::optional<ConfigRevision> latest() const;
  std::optional<ConfigRevision> get(std::uint32_t revision) const;
  // Appends revision latest+1.
  Result<ConfigRevision> append(const talos::server::v1::ConfigDocument& doc,
                                const std::string& principal, Timestamp created_at);

 private:
  explicit SqliteConfigStore(sqlite3* db) : db_(db) {}
  std::optional<ConfigRevision> query(const char* sql, std::int64_t arg, bool bind) const;
  sqlite3* db_;
};

}  // namespace talos::server
