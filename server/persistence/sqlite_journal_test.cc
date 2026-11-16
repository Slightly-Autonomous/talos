#include "server/persistence/sqlite_journal.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>

#include "core/journal/journal_conformance.h"

namespace talos::server {
namespace {

std::string temp_db() {
  static std::atomic<int> n{0};
  const char* dir = std::getenv("TEST_TMPDIR");
  auto path = std::filesystem::path(dir ? dir : "/tmp") /
              ("journal_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".db");
  std::filesystem::remove(path);
  return path.string();
}

struct SqliteFactory {
  static std::unique_ptr<IJournal> make() { return SqliteJournal::open(temp_db()).value(); }
  static std::unique_ptr<IJournal> reopen(IJournal& old) {
    return SqliteJournal::open(static_cast<SqliteJournal&>(old).path()).value();
  }
};

TEST(SqliteJournal, RefusesCorruptRows) {
  const std::string path = temp_db();
  {
    auto j = SqliteJournal::open(path).value();
    (void)j->append(Timestamp{1}, talos::testing::conformance_events()[0]);
  }
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "UPDATE journal SET entry = x'ffff00';", nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(db);
  auto reopened = SqliteJournal::open(path);
  ASSERT_FALSE(reopened.ok());
  EXPECT_EQ(reopened.error().code, "corrupt_journal");
}

TEST(SqliteJournal, RefusesSequenceGaps) {
  const std::string path = temp_db();
  {
    auto j = SqliteJournal::open(path).value();
    for (int i = 0; i < 3; ++i)
      (void)j->append(Timestamp{i}, talos::testing::conformance_events()[0]);
  }
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(db, "DELETE FROM journal WHERE journal_seq = 2;", nullptr, nullptr, nullptr),
      SQLITE_OK);
  sqlite3_close(db);
  EXPECT_EQ(SqliteJournal::open(path).error().code, "corrupt_journal");
}

TEST(SqliteJournal, RefusesNewerSchema) {
  const std::string path = temp_db();
  {
    (void)SqliteJournal::open(path).value();
  }
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=99;", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
  EXPECT_EQ(SqliteJournal::open(path).error().code, "schema_too_new");
}

}  // namespace
}  // namespace talos::server

namespace talos::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(Sqlite, JournalConformance, talos::server::SqliteFactory);
}  // namespace talos::testing
