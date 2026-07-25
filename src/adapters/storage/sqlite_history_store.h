/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "model/identity.h"
#include "model/timeline.h"
#include "protocol/dedupe_window.h"

#include <cstdint>
#include <filesystem>
#include <vector>

struct sqlite3;

namespace lora::adapters::storage {

inline constexpr std::uint32_t kHistoryApplicationId = 0x4c4d5347U; // "LMSG"
inline constexpr int kHistorySchemaVersion = 3;

enum class HistoryError {
    None,
    InvalidPath,
    OpenFailed,
    ConfigureFailed,
    CorruptDatabase,
    WrongApplication,
    UnsupportedSchema,
    MigrationFailed,
    IdentityMismatch,
    InvalidData,
    InvalidSnapshot,
    ReadFailed,
    WriteFailed,
};

enum class SqliteHistoryWritePoint {
    MigrationPostsRenamed,
    MigrationSeenTableCreated,
    SnapshotTimelineWritten,
    SnapshotDedupeWritten,
};

class ISqliteHistoryFailpoint {
public:
    virtual ~ISqliteHistoryFailpoint() = default;
    virtual bool should_fail(SqliteHistoryWritePoint point) noexcept = 0;
};

struct HistorySnapshot {
    std::vector<model::TimelineEntry> entries;
    std::uint64_t last_assigned_order = 0;
    std::vector<protocol::SeenMessageRecord> seen_messages;
    std::uint64_t last_seen_order = 0;
};

// Validation copies use this deterministic, app-owned sibling directory so an
// interrupted probe is discoverable and removable on the next locked startup.
std::filesystem::path history_probe_directory_path(
    const std::filesystem::path& database_path);
HistoryError discard_history_probe(
    const std::filesystem::path& database_path);

class SqliteHistoryStore {
public:
    // A non-null failpoint is retained for migration and snapshot-write tests
    // and must outlive the returned store.
    static core::Result<SqliteHistoryStore, HistoryError> open(
        const std::filesystem::path& database_path,
        ISqliteHistoryFailpoint* failpoint = nullptr);

    // Existing application history is inspected through a read-only connection,
    // including its exact schema, rows, and identity binding, before any
    // migration, journal-mode change, chmod, or other write is attempted.
    static core::Result<SqliteHistoryStore, HistoryError> open_existing_bound(
        const std::filesystem::path& database_path,
        const model::Identity& identity);

    SqliteHistoryStore(SqliteHistoryStore&& other) noexcept;
    SqliteHistoryStore& operator=(SqliteHistoryStore&& other) noexcept;
    ~SqliteHistoryStore();

    SqliteHistoryStore(const SqliteHistoryStore&) = delete;
    SqliteHistoryStore& operator=(const SqliteHistoryStore&) = delete;

    core::Result<HistorySnapshot, HistoryError> load_snapshot(
        const model::Identity& identity);

    // This adapter is a single-owner full-snapshot boundary. After
    // initialization, callers must derive the next snapshot from the current
    // loaded ledger and preserve its monotonic transition; PersistentSession is
    // the production owner that enforces this contract.
    HistoryError save_snapshot(const HistorySnapshot& snapshot,
                               const model::Identity& identity);
    HistoryError save_snapshot(const model::Timeline& timeline,
                               const model::Identity& identity);

    const std::filesystem::path& database_path() const noexcept;

private:
    SqliteHistoryStore(sqlite3* database,
                       std::filesystem::path database_path,
                       ISqliteHistoryFailpoint* failpoint);

    sqlite3* database_ = nullptr;
    std::filesystem::path database_path_;
    ISqliteHistoryFailpoint* failpoint_ = nullptr;
};

} // namespace lora::adapters::storage
