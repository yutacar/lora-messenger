/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/sqlite_history_store.h"

#include "../unit/test_model_helpers.h"
#include "../unit/test_support.h"
#include "sqlite3.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using lora::adapters::storage::HistoryError;
using lora::adapters::storage::HistorySnapshot;
using lora::adapters::storage::ISqliteHistoryFailpoint;
using lora::adapters::storage::SqliteHistoryStore;
using lora::adapters::storage::SqliteHistoryWritePoint;
using lora::model::Identity;
using lora::model::LocalDelivery;
using lora::model::LocalDeliveryState;
using lora::model::ReceivedOrigin;
using lora::model::TimelineEntry;

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view name)
        : directory_(std::filesystem::canonical(
                         std::filesystem::temp_directory_path()) /
                     ("lora-history-" + std::to_string(::getpid()))),
          path_(directory_ / (std::string(name) + ".sqlite3")) {
        remove_files();
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
    }

    ~TemporaryDatabase() {
        remove_files();
        std::error_code error;
        std::filesystem::remove(directory_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void remove_files() noexcept {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
        std::filesystem::remove(path_.string() + "-journal", error);
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

class SnapshotFailpoint final : public ISqliteHistoryFailpoint {
public:
    void arm(SqliteHistoryWritePoint point) noexcept {
        armed_ = point;
    }

    bool should_fail(
        SqliteHistoryWritePoint point) noexcept override {
        if (!armed_ || *armed_ != point) {
            return false;
        }
        armed_.reset();
        return true;
    }

private:
    std::optional<SqliteHistoryWritePoint> armed_;
};

Identity make_identity(std::uint8_t discriminator = 0x30,
                       std::uint64_t last_sequence =
                           std::numeric_limits<std::uint64_t>::max()) {
    auto user_id = lora::core::UserId::create("mika");
    return Identity::restore(lora::test::make_install_id(discriminator),
                             std::move(user_id).value(), last_sequence);
}

TimelineEntry received_entry(std::uint8_t message_discriminator,
                             std::uint64_t order) {
    auto post = lora::test::make_post(
        message_discriminator, 0x60,
        std::numeric_limits<std::uint64_t>::max(), u8"受信 hello",
        {lora::test::make_install_id(0x30),
         lora::test::make_install_id(0x40)},
        std::nullopt, std::numeric_limits<std::int64_t>::min(), "alice");
    return TimelineEntry{std::move(post), order, ReceivedOrigin{}};
}

TimelineEntry local_entry(std::uint8_t message_discriminator,
                          std::uint64_t sender_sequence,
                          std::uint64_t order,
                          LocalDeliveryState state,
                          std::optional<lora::core::MessageId> reply_to =
                              std::nullopt) {
    auto post = lora::test::make_post(
        message_discriminator, 0x30, sender_sequence, "local body",
        {lora::test::make_install_id(0x40)}, std::move(reply_to),
        std::numeric_limits<std::int64_t>::max(), "mika");
    return TimelineEntry{std::move(post), order, LocalDelivery{state}};
}

bool exec_sql(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool scalar_integer(const std::filesystem::path& path, const char* sql,
                    sqlite3_int64& value) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK;
    const bool read =
        prepared && sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) == SQLITE_INTEGER;
    if (read) {
        value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return read;
}

bool scalar_text(const std::filesystem::path& path, const char* sql,
                 std::string& value) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK;
    const bool read =
        prepared && sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) == SQLITE_TEXT;
    if (read) {
        const auto* text =
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const int bytes = sqlite3_column_bytes(statement, 0);
        value.assign(text, static_cast<std::size_t>(bytes));
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return read;
}

bool mutate_database(const std::filesystem::path& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    const bool result = exec_sql(database, sql);
    sqlite3_close(database);
    return result;
}

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
}

void check_entries_equal(lora::test::Runner& runner,
                         const TimelineEntry& expected,
                         const TimelineEntry& actual) {
    CHECK_EQ(expected.post, actual.post);
    CHECK_EQ(expected.received_order, actual.received_order);
    CHECK_EQ(expected.origin, actual.origin);
}

} // namespace

int main() {
    lora::test::Runner runner;

    runner.run("new database uses the bounded strict WAL schema", [&] {
        TemporaryDatabase temporary("schema");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);

        sqlite3_int64 integer_value = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA application_id;",
                             integer_value));
        CHECK_EQ(integer_value,
                 static_cast<sqlite3_int64>(
                     lora::adapters::storage::kHistoryApplicationId));
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;",
                             integer_value));
        CHECK_EQ(integer_value, 3);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM pragma_table_list"
            " WHERE schema='main' AND name IN"
            " ('meta','posts','mentions','seen_messages')"
            " AND strict=1;",
            integer_value));
        CHECK_EQ(integer_value, 4);

        std::string text_value;
        CHECK(scalar_text(temporary.path(), "PRAGMA journal_mode;", text_value));
        CHECK_EQ(text_value, "wal");

        std::error_code permissions_error;
        const auto permissions =
            std::filesystem::status(temporary.path(), permissions_error)
                .permissions();
        CHECK(!permissions_error);
        CHECK((permissions & (std::filesystem::perms::group_all |
                              std::filesystem::perms::others_all)) ==
              std::filesystem::perms::none);

        auto empty = opened.value().load_snapshot(make_identity());
        REQUIRE(empty);
        CHECK(empty.value().entries.empty());
        CHECK_EQ(empty.value().last_assigned_order, 0U);
        CHECK(empty.value().seen_messages.empty());
        CHECK_EQ(empty.value().last_seen_order, 0U);
    });

    runner.run("round trip preserves every post field and uint64 extremes", [&] {
        TemporaryDatabase temporary("round-trip");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto identity = make_identity();

        HistorySnapshot expected;
        expected.last_assigned_order =
            std::numeric_limits<std::uint64_t>::max();
        expected.entries.push_back(received_entry(0x10, 1));
        expected.entries.push_back(local_entry(
            0x11, std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(),
            LocalDeliveryState::Unknown,
            expected.entries.front().post.message_id()));

        CHECK_EQ(opened.value().save_snapshot(expected, identity),
                 HistoryError::None);
        auto loaded = opened.value().load_snapshot(identity);
        REQUIRE(loaded);
        CHECK_EQ(loaded.value().last_assigned_order,
                 expected.last_assigned_order);
        REQUIRE(loaded.value().entries.size() == expected.entries.size());
        CHECK_EQ(loaded.value().seen_messages.size(),
                 expected.entries.size());
        CHECK_EQ(loaded.value().last_seen_order,
                 expected.entries.size());
        check_entries_equal(runner, expected.entries[0],
                            loaded.value().entries[0]);
        check_entries_equal(runner, expected.entries[1],
                            loaded.value().entries[1]);

        std::string sequence_hex;
        CHECK(scalar_text(
            temporary.path(),
            "SELECT hex(sender_sequence) FROM posts"
            " WHERE origin=1;",
            sequence_hex));
        CHECK_EQ(sequence_hex, "FFFFFFFFFFFFFFFF");
    });

    runner.run("all local delivery states are durable", [&] {
        TemporaryDatabase temporary("states");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto identity = make_identity(0x30, 4);

        HistorySnapshot snapshot;
        snapshot.last_assigned_order = 4;
        snapshot.entries.push_back(
            local_entry(0x10, 1, 1, LocalDeliveryState::Queued));
        snapshot.entries.push_back(
            local_entry(0x11, 2, 2, LocalDeliveryState::Broadcast));
        snapshot.entries.push_back(
            local_entry(0x12, 3, 3, LocalDeliveryState::Failed));
        snapshot.entries.push_back(
            local_entry(0x13, 4, 4, LocalDeliveryState::Unknown));
        REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                HistoryError::None);

        auto loaded = opened.value().load_snapshot(identity);
        REQUIRE(loaded);
        REQUIRE(loaded.value().entries.size() == 4);
        for (std::size_t index = 0; index < 4; ++index) {
            check_entries_equal(runner, snapshot.entries[index],
                                loaded.value().entries[index]);
        }
    });

    runner.run("normal snapshot failpoints roll back timeline and dedupe",
               [&] {
        TemporaryDatabase temporary("snapshot-rollback");
        SnapshotFailpoint failpoint;
        auto opened =
            SqliteHistoryStore::open(temporary.path(), &failpoint);
        REQUIRE(opened);
        auto identity = make_identity(0x30, 1);

        HistorySnapshot baseline;
        baseline.last_assigned_order = 1;
        baseline.entries.push_back(local_entry(
            0x10, 1, 1, LocalDeliveryState::Broadcast));
        REQUIRE(opened.value().save_snapshot(baseline, identity) ==
                HistoryError::None);

        HistorySnapshot candidate = baseline;
        candidate.last_assigned_order = 2;
        candidate.entries.push_back(received_entry(0x11, 2));
        for (const auto point : {
                 SqliteHistoryWritePoint::SnapshotTimelineWritten,
                 SqliteHistoryWritePoint::SnapshotDedupeWritten}) {
            failpoint.arm(point);
            CHECK_EQ(opened.value().save_snapshot(
                         candidate, identity),
                     HistoryError::WriteFailed);
            auto loaded = opened.value().load_snapshot(identity);
            REQUIRE(loaded);
            CHECK_EQ(loaded.value().last_assigned_order, 1U);
            REQUIRE(loaded.value().entries.size() == 1U);
            CHECK_EQ(loaded.value().entries.front().post,
                     baseline.entries.front().post);
            CHECK_EQ(loaded.value().seen_messages.size(), 1U);
            CHECK_EQ(loaded.value().last_seen_order, 1U);
        }
    });

    runner.run("database is permanently bound to one install identity", [&] {
        TemporaryDatabase temporary("identity");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto first = make_identity(0x30, 1);
        auto second = make_identity(0x31, 1);

        HistorySnapshot snapshot;
        snapshot.last_assigned_order = 1;
        snapshot.entries.push_back(
            local_entry(0x10, 1, 1, LocalDeliveryState::Queued));
        REQUIRE(opened.value().save_snapshot(snapshot, first) ==
                HistoryError::None);

        auto wrong_load = opened.value().load_snapshot(second);
        CHECK(!wrong_load);
        CHECK_EQ(wrong_load.error(), HistoryError::IdentityMismatch);
        CHECK_EQ(opened.value().save_snapshot(HistorySnapshot{}, second),
                 HistoryError::IdentityMismatch);
    });

    runner.run("snapshot validation rejects overflow and inconsistent state", [&] {
        TemporaryDatabase temporary("validation");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto identity = make_identity(0x30, 1);

        HistorySnapshot too_many;
        too_many.last_assigned_order = 1;
        too_many.entries.assign(
            lora::core::kMaxTimelineEntries + 1,
            local_entry(0x10, 1, 1, LocalDeliveryState::Queued));
        CHECK_EQ(opened.value().save_snapshot(too_many, identity),
                 HistoryError::InvalidSnapshot);

        HistorySnapshot order_too_new;
        order_too_new.last_assigned_order = 0;
        order_too_new.entries.push_back(
            local_entry(0x10, 1, 1, LocalDeliveryState::Queued));
        CHECK_EQ(opened.value().save_snapshot(order_too_new, identity),
                 HistoryError::InvalidSnapshot);

        HistorySnapshot duplicate_order;
        duplicate_order.last_assigned_order = 1;
        duplicate_order.entries.push_back(received_entry(0x10, 1));
        duplicate_order.entries.push_back(received_entry(0x11, 1));
        CHECK_EQ(opened.value().save_snapshot(duplicate_order, identity),
                 HistoryError::InvalidSnapshot);

        HistorySnapshot duplicate_local_sequence;
        duplicate_local_sequence.last_assigned_order = 2;
        duplicate_local_sequence.entries.push_back(
            local_entry(0x12, 1, 1, LocalDeliveryState::Queued));
        duplicate_local_sequence.entries.push_back(
            local_entry(0x13, 1, 2, LocalDeliveryState::Broadcast));
        CHECK_EQ(opened.value().save_snapshot(duplicate_local_sequence,
                                              make_identity(0x30, 2)),
                 HistoryError::InvalidSnapshot);

        HistorySnapshot decreasing_local_sequence;
        decreasing_local_sequence.last_assigned_order = 2;
        decreasing_local_sequence.entries.push_back(
            local_entry(0x14, 2, 1, LocalDeliveryState::Queued));
        decreasing_local_sequence.entries.push_back(
            local_entry(0x15, 1, 2, LocalDeliveryState::Broadcast));
        CHECK_EQ(opened.value().save_snapshot(decreasing_local_sequence,
                                              make_identity(0x30, 2)),
                 HistoryError::InvalidSnapshot);
    });

    runner.run("bound open rejects duplicate and decreasing local sequences",
               [&] {
        struct SequenceDamage {
            const char* name;
            const char* sql;
        };
        constexpr std::array<SequenceDamage, 2> damage_cases{{
            {"duplicate",
             "UPDATE posts SET sender_sequence=X'0000000000000001'"
             " WHERE received_order=X'0000000000000002';"},
            {"decreasing",
             "UPDATE posts SET sender_sequence=X'0000000000000003'"
             " WHERE received_order=X'0000000000000001';"},
        }};

        for (const auto& damage : damage_cases) {
            TemporaryDatabase temporary(
                std::string("local-sequence-") + damage.name);
            auto identity = make_identity(0x30, 3);
            {
                auto opened = SqliteHistoryStore::open(temporary.path());
                REQUIRE(opened);
                HistorySnapshot snapshot;
                snapshot.last_assigned_order = 2;
                snapshot.entries.push_back(
                    local_entry(0x10, 1, 1,
                                LocalDeliveryState::Queued));
                snapshot.entries.push_back(
                    local_entry(0x11, 2, 2,
                                LocalDeliveryState::Broadcast));
                REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                        HistoryError::None);
            }

            REQUIRE(mutate_database(temporary.path(), damage.sql));
            const auto damaged = read_bytes(temporary.path());
            REQUIRE(!damaged.empty());

            auto reopened = SqliteHistoryStore::open_existing_bound(
                temporary.path(), identity);
            CHECK(!reopened);
            CHECK_EQ(reopened.error(), HistoryError::InvalidData);
            CHECK_EQ(read_bytes(temporary.path()), damaged);
        }
    });

    runner.run("bound open rejects an over-capacity queued set",
               [&] {
        TemporaryDatabase temporary("queued-overflow");
        auto identity = make_identity(
            0x30, lora::core::kMaxQueuedLocalPosts + 1U);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            HistorySnapshot snapshot;
            snapshot.last_assigned_order =
                lora::core::kMaxQueuedLocalPosts + 1U;
            snapshot.entries.push_back(local_entry(
                0x10, 1U, 1U,
                LocalDeliveryState::Broadcast));
            for (std::size_t index = 0;
                 index < lora::core::kMaxQueuedLocalPosts; ++index) {
                const auto value =
                    static_cast<std::uint64_t>(index + 2U);
                snapshot.entries.push_back(local_entry(
                    static_cast<std::uint8_t>(0x11U + index),
                    value, value,
                    LocalDeliveryState::Queued));
            }
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
            auto invalid_snapshot = snapshot;
            std::get<LocalDelivery>(
                invalid_snapshot.entries.front().origin)
                .state = LocalDeliveryState::Queued;
            CHECK_EQ(opened.value().save_snapshot(
                         invalid_snapshot, identity),
                     HistoryError::InvalidSnapshot);
        }

        REQUIRE(mutate_database(
            temporary.path(),
            "UPDATE posts SET local_state=0"
            " WHERE received_order=X'0000000000000001';"));
        const auto damaged = read_bytes(temporary.path());
        REQUIRE(!damaged.empty());

        auto reopened = SqliteHistoryStore::open_existing_bound(
            temporary.path(), identity);
        CHECK(!reopened);
        CHECK_EQ(reopened.error(), HistoryError::InvalidData);
        CHECK_EQ(read_bytes(temporary.path()), damaged);
    });

    runner.run("bound validation sees WAL state without changing originals", [&] {
        TemporaryDatabase temporary("wal-probe");
        auto identity = make_identity(0x30, 1);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            HistorySnapshot snapshot;
            snapshot.last_assigned_order = 1;
            snapshot.entries.push_back(
                local_entry(0x10, 1, 1, LocalDeliveryState::Broadcast));
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
        }

        sqlite3* writer = nullptr;
        REQUIRE(sqlite3_open_v2(
                    temporary.path().string().c_str(), &writer,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW,
                    nullptr) == SQLITE_OK);
        REQUIRE(exec_sql(writer, "PRAGMA wal_autocheckpoint=0;"));
        REQUIRE(exec_sql(
            writer,
            "UPDATE meta SET value=X'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'"
            " WHERE key='install_id';"));

        const auto wal_path =
            std::filesystem::path(temporary.path().string() + "-wal");
        const auto shm_path =
            std::filesystem::path(temporary.path().string() + "-shm");
        REQUIRE(std::filesystem::exists(wal_path));
        REQUIRE(std::filesystem::exists(shm_path));
        const auto main_before = read_bytes(temporary.path());
        const auto wal_before = read_bytes(wal_path);
        const auto shm_before = read_bytes(shm_path);
        REQUIRE(!wal_before.empty());

        auto reopened = SqliteHistoryStore::open_existing_bound(
            temporary.path(), identity);
        CHECK(!reopened);
        CHECK_EQ(reopened.error(), HistoryError::IdentityMismatch);
        CHECK_EQ(read_bytes(temporary.path()), main_before);
        CHECK_EQ(read_bytes(wal_path), wal_before);
        CHECK_EQ(read_bytes(shm_path), shm_before);
        CHECK_EQ(sqlite3_close(writer), SQLITE_OK);
    });

    runner.run("schema tampering blocks replacement without data loss", [&] {
        TemporaryDatabase temporary("rollback");
        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto identity = make_identity(0x30, 2);

        HistorySnapshot original;
        original.last_assigned_order = 1;
        original.entries.push_back(
            local_entry(0x10, 1, 1, LocalDeliveryState::Queued));
        REQUIRE(opened.value().save_snapshot(original, identity) ==
                HistoryError::None);

        REQUIRE(mutate_database(
            temporary.path(),
            "CREATE TRIGGER sqlitex_rewrite AFTER INSERT ON posts"
            " BEGIN UPDATE posts SET body='rewritten'"
            " WHERE message_id=NEW.message_id; END;"));

        HistorySnapshot replacement;
        replacement.last_assigned_order = 2;
        replacement.entries.push_back(
            local_entry(0x11, 2, 2, LocalDeliveryState::Unknown));
        CHECK_EQ(opened.value().save_snapshot(replacement, identity),
                 HistoryError::InvalidData);

        REQUIRE(mutate_database(temporary.path(),
                                "DROP TRIGGER sqlitex_rewrite;"));
        auto loaded = opened.value().load_snapshot(identity);
        REQUIRE(loaded);
        REQUIRE(loaded.value().entries.size() == 1);
        check_entries_equal(runner, original.entries.front(),
                            loaded.value().entries.front());
    });

    runner.run("initialized dedupe ledger cannot be silently reseeded",
               [&] {
        TemporaryDatabase temporary("dedupe-reset");
        auto identity = make_identity(0x30, 1);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            HistorySnapshot snapshot;
            snapshot.last_assigned_order = 1;
            snapshot.entries.push_back(
                local_entry(0x10, 1, 1,
                            LocalDeliveryState::Broadcast));
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
        }

        REQUIRE(mutate_database(
            temporary.path(),
            "DELETE FROM seen_messages;"
            "UPDATE meta SET value=X'0000000000000000'"
            " WHERE key='last_seen_order';"));
        const auto damaged = read_bytes(temporary.path());
        REQUIRE(!damaged.empty());

        auto reopened = SqliteHistoryStore::open_existing_bound(
            temporary.path(), identity);
        CHECK(!reopened);
        CHECK_EQ(reopened.error(), HistoryError::InvalidData);
        CHECK_EQ(read_bytes(temporary.path()), damaged);
    });

    runner.run("dedupe high-water cannot exceed newest retained record",
               [&] {
        TemporaryDatabase temporary("dedupe-high-water");
        auto identity = make_identity(0x30, 1);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            HistorySnapshot snapshot;
            snapshot.last_assigned_order = 1;
            snapshot.entries.push_back(
                local_entry(0x10, 1, 1,
                            LocalDeliveryState::Broadcast));
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
        }

        REQUIRE(mutate_database(
            temporary.path(),
            "UPDATE meta SET value=X'FFFFFFFFFFFFFFFF'"
            " WHERE key='last_seen_order';"));
        const auto damaged = read_bytes(temporary.path());
        REQUIRE(!damaged.empty());

        auto reopened = SqliteHistoryStore::open_existing_bound(
            temporary.path(), identity);
        CHECK(!reopened);
        CHECK_EQ(reopened.error(), HistoryError::InvalidData);
        CHECK_EQ(read_bytes(temporary.path()), damaged);
    });

    return runner.finish();
}
