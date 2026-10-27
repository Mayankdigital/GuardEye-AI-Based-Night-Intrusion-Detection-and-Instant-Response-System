#include "event_store.h"

#include <sqlite3.h>

#include <algorithm>

#include "json.hpp"
#include "log.h"
#include "util.h"

namespace guardeye {

EventStore::~EventStore() { if (db_) sqlite3_close(db_); }

bool EventStore::open(const std::string& path, std::string& err) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    err = std::string("sqlite open failed: ") + (db_ ? sqlite3_errmsg(db_) : "?");
    return false;
  }
  const char* sql =
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "CREATE TABLE IF NOT EXISTS events("
      " id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, type TEXT NOT NULL,"
      " detail TEXT, confidence REAL, snapshot TEXT, clip TEXT);"
      "CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts);";
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    err = std::string("sqlite init failed: ") + (msg ? msg : "?");
    sqlite3_free(msg);
    return false;
  }
  return true;
}

int64_t EventStore::add(std::time_t ts, const std::string& type, const std::string& detail,
                        double confidence, const std::string& snapshot, const std::string& clip) {
  std::lock_guard<std::mutex> lk(m_);
  if (!db_) return -1;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "INSERT INTO events(ts,type,detail,confidence,snapshot,clip) VALUES(?,?,?,?,?,?)",
                         -1, &st, nullptr) != SQLITE_OK) return -1;
  sqlite3_bind_int64(st, 1, ts);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, detail.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 4, confidence);
  sqlite3_bind_text(st, 5, snapshot.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, clip.c_str(), -1, SQLITE_TRANSIENT);
  int64_t id = -1;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

void EventStore::setClip(int64_t id, const std::string& clip) {
  std::lock_guard<std::mutex> lk(m_);
  if (!db_ || id < 0) return;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "UPDATE events SET clip=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, clip.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::string EventStore::recentJson(int limit) {
  limit = std::clamp(limit, 1, 500);
  nlohmann::json arr = nlohmann::json::array();
  std::lock_guard<std::mutex> lk(m_);
  if (!db_) return arr.dump();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT id,ts,type,detail,confidence,snapshot,clip FROM events ORDER BY id DESC LIMIT ?",
                         -1, &st, nullptr) != SQLITE_OK) return arr.dump();
  sqlite3_bind_int(st, 1, limit);
  auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return t ? std::string(reinterpret_cast<const char*>(t)) : std::string(); };
  while (sqlite3_step(st) == SQLITE_ROW) {
    std::time_t ts = static_cast<std::time_t>(sqlite3_column_int64(st, 1));
    arr.push_back({{"id", sqlite3_column_int64(st, 0)}, {"ts", ts}, {"time", formatLocal(ts)},
                   {"type", txt(2)}, {"detail", txt(3)}, {"confidence", sqlite3_column_double(st, 4)},
                   {"snapshot", txt(5)}, {"clip", txt(6)}});
  }
  sqlite3_finalize(st);
  return arr.dump();
}

int64_t EventStore::count() {
  std::lock_guard<std::mutex> lk(m_);
  if (!db_) return 0;
  sqlite3_stmt* st = nullptr;
  int64_t n = 0;
  if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM events", -1, &st, nullptr) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return n;
}

}  // namespace guardeye
