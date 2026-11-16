// Durable IJournal on SQLite (ADR 0010).
//
// One row per entry: journal_seq is the INTEGER PRIMARY KEY, the entry is the
// serialized talos.v1.JournalEntry. WAL mode with synchronous=FULL so an
// acknowledged append survives power loss. open() verifies integrity and that
// every row decodes, and refuses to start otherwise (fail closed).
#pragma once

#include <memory>
#include <string>

#include "core/base/result.h"
#include "core/journal/journal.h"

struct sqlite3;

namespace talos::server {

class SqliteJournal final : public IJournal {
 public:
  static Result<std::unique_ptr<SqliteJournal>> open(const std::string& path);
  ~SqliteJournal() override;
  SqliteJournal(const SqliteJournal&) = delete;
  SqliteJournal& operator=(const SqliteJournal&) = delete;

  Result<JournalEntry> append(Timestamp received_at, DomainEvent event) override;
  std::vector<JournalEntry> read(std::uint64_t after_seq, std::size_t limit) const override;
  std::uint64_t last_seq() const override { return last_seq_; }

  const std::string& path() const { return path_; }

 private:
  SqliteJournal(sqlite3* db, std::string path) : db_(db), path_(std::move(path)) {}
  Status initialize();

  sqlite3* db_;
  std::string path_;
  std::uint64_t last_seq_ = 0;
  Timestamp last_received_;
};

}  // namespace talos::server
