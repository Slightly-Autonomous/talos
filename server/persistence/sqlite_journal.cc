#include "server/persistence/sqlite_journal.h"

#include <sqlite3.h>

#include <limits>

#include "codec/codec.h"

namespace talos::server {

namespace {

constexpr int kSchemaVersion = 1;

Error sql_error(sqlite3* db, const std::string& what) {
  return {"storage_error", what + ": " + (db ? sqlite3_errmsg(db) : "no database")};
}

// RAII prepared statement.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    ok_ = sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) == SQLITE_OK;
  }
  ~Stmt() { sqlite3_finalize(s_); }
  bool ok() const { return ok_; }
  sqlite3_stmt* get() const { return s_; }

 private:
  sqlite3_stmt* s_ = nullptr;
  bool ok_ = false;
};

Status exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown";
    sqlite3_free(err);
    return Error{"storage_error", std::string(sql).substr(0, 40) + ": " + msg};
  }
  return Status::Ok();
}

const char* kind_of(const DomainEvent& e) {
  if (std::holds_alternative<ObservationRecorded>(e)) return "observation";
  if (std::holds_alternative<CommandTransition>(e)) return "command_transition";
  return "arm_mode_changed";
}

Result<JournalEntry> decode_row(sqlite3_stmt* s) {
  const void* blob = sqlite3_column_blob(s, 0);
  const int size = sqlite3_column_bytes(s, 0);
  talos::v1::JournalEntry p;
  if (!p.ParseFromArray(blob, size)) {
    return Error{"corrupt_journal",
                 "undecodable entry at seq " + std::to_string(sqlite3_column_int64(s, 1))};
  }
  auto e = codec::from_proto(p);
  if (!e) return e.error();
  if (e.value().journal_seq != static_cast<std::uint64_t>(sqlite3_column_int64(s, 1))) {
    return Error{"corrupt_journal",
                 "seq mismatch at row " + std::to_string(sqlite3_column_int64(s, 1))};
  }
  return e;
}

}  // namespace

Result<std::unique_ptr<SqliteJournal>> SqliteJournal::open(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    Error e = sql_error(db, "open " + path);
    sqlite3_close(db);
    return e;
  }
  std::unique_ptr<SqliteJournal> j(new SqliteJournal(db, path));
  if (Status s = j->initialize(); !s) return s.error();
  return j;
}

SqliteJournal::~SqliteJournal() { sqlite3_close(db_); }

Status SqliteJournal::initialize() {
  for (const char* pragma :
       {"PRAGMA journal_mode=WAL;", "PRAGMA synchronous=FULL;", "PRAGMA foreign_keys=ON;"}) {
    if (Status s = exec(db_, pragma); !s) return s;
  }
  int version = 0;
  {
    Stmt v(db_, "PRAGMA user_version;");
    if (v.ok() && sqlite3_step(v.get()) == SQLITE_ROW) version = sqlite3_column_int(v.get(), 0);
  }
  if (version > kSchemaVersion) {
    return Error{"schema_too_new", "journal schema v" + std::to_string(version) +
                                       " is newer than this hub (v" +
                                       std::to_string(kSchemaVersion) + ")"};
  }
  if (version == 0) {
    if (Status s = exec(db_,
                        "CREATE TABLE IF NOT EXISTS journal ("
                        "  journal_seq INTEGER PRIMARY KEY,"
                        "  received_at_ms INTEGER NOT NULL,"
                        "  clock_regressed INTEGER NOT NULL,"
                        "  kind TEXT NOT NULL,"
                        "  entry BLOB NOT NULL);"
                        "PRAGMA user_version=1;");
        !s) {
      return s;
    }
  }
  {
    Stmt check(db_, "PRAGMA quick_check;");
    if (!check.ok() || sqlite3_step(check.get()) != SQLITE_ROW ||
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(check.get(), 0))) != "ok") {
      return Error{"corrupt_journal", "sqlite quick_check failed for " + path_};
    }
  }
  // Every row must decode and seqs must be contiguous from 1.
  Stmt all(db_, "SELECT entry, journal_seq, received_at_ms FROM journal ORDER BY journal_seq;");
  if (!all.ok()) return sql_error(db_, "scan");
  std::uint64_t expect = 1;
  while (sqlite3_step(all.get()) == SQLITE_ROW) {
    auto e = decode_row(all.get());
    if (!e) return e.error();
    if (e.value().journal_seq != expect) {
      return Error{"corrupt_journal", "gap before seq " + std::to_string(e.value().journal_seq)};
    }
    ++expect;
    last_seq_ = e.value().journal_seq;
    last_received_ = e.value().received_at;
  }
  return Status::Ok();
}

Result<JournalEntry> SqliteJournal::append(Timestamp received_at, DomainEvent event) {
  JournalEntry e;
  e.journal_seq = last_seq_ + 1;
  e.received_at = received_at;
  if (last_seq_ > 0 && received_at < last_received_) {
    e.received_at = last_received_;
    e.clock_regressed = true;
  }
  e.event = std::move(event);

  std::string bytes;
  if (!codec::to_proto(e).SerializeToString(&bytes)) {
    return Error{"encode_error", "could not serialize journal entry"};
  }
  Stmt ins(db_,
           "INSERT INTO journal(journal_seq, received_at_ms, clock_regressed, kind, entry) "
           "VALUES (?, ?, ?, ?, ?);");
  if (!ins.ok()) return sql_error(db_, "prepare insert");
  sqlite3_bind_int64(ins.get(), 1, static_cast<sqlite3_int64>(e.journal_seq));
  sqlite3_bind_int64(ins.get(), 2, e.received_at.unix_ms);
  sqlite3_bind_int(ins.get(), 3, e.clock_regressed ? 1 : 0);
  sqlite3_bind_text(ins.get(), 4, kind_of(e.event), -1, SQLITE_STATIC);
  sqlite3_bind_blob(ins.get(), 5, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
  if (sqlite3_step(ins.get()) != SQLITE_DONE) return sql_error(db_, "insert");
  last_seq_ = e.journal_seq;
  last_received_ = e.received_at;
  return e;
}

std::vector<JournalEntry> SqliteJournal::read(std::uint64_t after_seq, std::size_t limit) const {
  std::vector<JournalEntry> out;
  Stmt q(db_,
         "SELECT entry, journal_seq FROM journal WHERE journal_seq > ? ORDER BY journal_seq "
         "LIMIT ?;");
  if (!q.ok()) return out;
  sqlite3_bind_int64(q.get(), 1, static_cast<sqlite3_int64>(after_seq));
  const auto max = static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max());
  sqlite3_bind_int64(q.get(), 2, static_cast<sqlite3_int64>(limit > max ? max : limit));
  while (sqlite3_step(q.get()) == SQLITE_ROW) {
    auto e = decode_row(q.get());
    if (!e) break;  // open() verified every row; only our own writes follow
    out.push_back(std::move(e).value());
  }
  return out;
}

}  // namespace talos::server
