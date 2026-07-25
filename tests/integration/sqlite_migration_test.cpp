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
using lora::model::TimelineEntry;

constexpr const char* kV1Schema = R"sql(
PRAGMA application_id = 1280136007;
PRAGMA user_version = 1;
CREATE TABLE meta (
    key TEXT PRIMARY KEY NOT NULL
        CHECK(key IN ('install_id', 'last_assigned_order')),
    value BLOB NOT NULL,
    CHECK(
        (key = 'install_id' AND length(value) = 16) OR
        (key = 'last_assigned_order' AND length(value) = 8)
    )
) WITHOUT ROWID, STRICT;
CREATE TABLE posts (
    message_id BLOB PRIMARY KEY NOT NULL CHECK(length(message_id) = 16),
    sender_id BLOB NOT NULL CHECK(length(sender_id) = 16),
    sender_sequence BLOB NOT NULL
        CHECK(length(sender_sequence) = 8
              AND sender_sequence <> X'0000000000000000'),
    sender_user_id TEXT NOT NULL
        CHECK(length(CAST(sender_user_id AS BLOB)) BETWEEN 1 AND 24),
    body TEXT NOT NULL
        CHECK(length(CAST(body AS BLOB)) BETWEEN 1 AND 160),
    reply_to BLOB
        CHECK(reply_to IS NULL OR
              (length(reply_to) = 16 AND reply_to <> message_id)),
    received_order BLOB NOT NULL UNIQUE
        CHECK(length(received_order) = 8
              AND received_order <> X'0000000000000000'),
    origin INTEGER NOT NULL CHECK(origin IN (0, 1)),
    local_state INTEGER,
    CHECK(
        (origin = 0 AND local_state IS NULL) OR
        (origin = 1 AND local_state BETWEEN 0 AND 3)
    )
) STRICT;
)sql";

constexpr const char* kUpgradeV1ToV2Sql = R"sql(
BEGIN IMMEDIATE;
ALTER TABLE posts RENAME TO posts_v1;
CREATE TABLE posts (
    message_id BLOB PRIMARY KEY NOT NULL
        CHECK(length(message_id) = 16),
    sender_id BLOB NOT NULL
        CHECK(length(sender_id) = 16),
    sender_sequence BLOB NOT NULL
        CHECK(length(sender_sequence) = 8
              AND sender_sequence <> X'0000000000000000'),
    sender_user_id TEXT NOT NULL
        CHECK(length(CAST(sender_user_id AS BLOB)) BETWEEN 1 AND 24),
    body TEXT NOT NULL
        CHECK(length(CAST(body AS BLOB)) BETWEEN 1 AND 160),
    reply_to BLOB
        CHECK(reply_to IS NULL OR
              (length(reply_to) = 16 AND reply_to <> message_id)),
    sender_time INTEGER,
    received_order BLOB NOT NULL UNIQUE
        CHECK(length(received_order) = 8
              AND received_order <> X'0000000000000000'),
    origin INTEGER NOT NULL
        CHECK(origin IN (0, 1)),
    local_state INTEGER,
    CHECK(
        (origin = 0 AND local_state IS NULL) OR
        (origin = 1 AND local_state BETWEEN 0 AND 3)
    )
) STRICT;
INSERT INTO posts(
    message_id, sender_id, sender_sequence, sender_user_id, body,
    reply_to, sender_time, received_order, origin, local_state
) SELECT message_id, sender_id, sender_sequence, sender_user_id, body,
         reply_to, NULL, received_order, origin, local_state
  FROM posts_v1;
DROP TABLE posts_v1;
CREATE TABLE mentions (
    message_id BLOB NOT NULL,
    ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 3),
    mentioned_install_id BLOB NOT NULL
        CHECK(length(mentioned_install_id) = 16),
    PRIMARY KEY(message_id, ordinal),
    UNIQUE(message_id, mentioned_install_id),
    FOREIGN KEY(message_id) REFERENCES posts(message_id) ON DELETE CASCADE
) WITHOUT ROWID, STRICT;
PRAGMA user_version = 2;
COMMIT;
)sql";

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view name)
        : directory_(std::filesystem::canonical(
                         std::filesystem::temp_directory_path()) /
                     ("lora-migration-" + std::to_string(::getpid()))),
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

Identity make_identity(std::uint64_t last_sequence = 7) {
    auto user_id = lora::core::UserId::create("mika");
    return Identity::restore(lora::test::make_install_id(0x30),
                             std::move(user_id).value(), last_sequence);
}

TimelineEntry make_local_entry(std::uint64_t sender_sequence = 7,
                               std::uint64_t order = 9) {
    auto post = lora::test::make_post(
        0x10, 0x30, sender_sequence, "migrated post", {}, std::nullopt,
        std::nullopt, "mika");
    return TimelineEntry{
        std::move(post), order, LocalDelivery{LocalDeliveryState::Failed}};
}

std::array<std::uint8_t, 8> encode_uint64(std::uint64_t value) {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = encoded.size(); index > 0; --index) {
        encoded[index - 1] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
    return encoded;
}

bool exec_sql(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool open_raw(const std::filesystem::path& path, sqlite3** database) {
    return sqlite3_open_v2(
               path.string().c_str(), database,
               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                   SQLITE_OPEN_NOFOLLOW,
               nullptr) == SQLITE_OK;
}

bool create_v1_fixture(const std::filesystem::path& path,
                       const Identity& identity,
                       const TimelineEntry& entry) {
    sqlite3* database = nullptr;
    if (!open_raw(path, &database) || !exec_sql(database, kV1Schema)) {
        sqlite3_close(database);
        return false;
    }

    sqlite3_stmt* meta = nullptr;
    sqlite3_stmt* post = nullptr;
    bool success =
        sqlite3_prepare_v2(
            database, "INSERT INTO meta(key,value) VALUES(?1,?2);", -1,
            &meta, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(
            database,
            "INSERT INTO posts("
            " message_id,sender_id,sender_sequence,sender_user_id,body,"
            " reply_to,received_order,origin,local_state"
            ") VALUES(?1,?2,?3,?4,?5,NULL,?6,1,2);",
            -1, &post, nullptr) == SQLITE_OK;

    const auto& install_id = identity.install_id().uuid().bytes();
    if (success) {
        success =
            sqlite3_bind_text(meta, 1, "install_id", -1, SQLITE_STATIC) ==
                SQLITE_OK &&
            sqlite3_bind_blob(meta, 2, install_id.data(),
                              static_cast<int>(install_id.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(meta) == SQLITE_DONE &&
            sqlite3_reset(meta) == SQLITE_OK &&
            sqlite3_clear_bindings(meta) == SQLITE_OK;
    }

    const auto order = encode_uint64(entry.received_order);
    if (success) {
        success =
            sqlite3_bind_text(meta, 1, "last_assigned_order", -1,
                              SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_blob(meta, 2, order.data(),
                              static_cast<int>(order.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(meta) == SQLITE_DONE;
    }

    const auto& message_id = entry.post.message_id().uuid().bytes();
    const auto& sender_id = entry.post.sender_id().uuid().bytes();
    const auto sequence = encode_uint64(entry.post.sender_sequence());
    const auto& user = entry.post.sender_user_id().value();
    const auto& body = entry.post.body().value();
    if (success) {
        success =
            sqlite3_bind_blob(post, 1, message_id.data(),
                              static_cast<int>(message_id.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_blob(post, 2, sender_id.data(),
                              static_cast<int>(sender_id.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_blob(post, 3, sequence.data(),
                              static_cast<int>(sequence.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(post, 4, user.data(),
                              static_cast<int>(user.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(post, 5, body.data(),
                              static_cast<int>(body.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_blob(post, 6, order.data(),
                              static_cast<int>(order.size()),
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(post) == SQLITE_DONE;
    }

    sqlite3_finalize(post);
    sqlite3_finalize(meta);
    sqlite3_close(database);
    return success;
}

bool create_v2_fixture(const std::filesystem::path& path) {
    const auto identity = make_identity();
    const auto entry = make_local_entry();
    if (!create_v1_fixture(path, identity, entry)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (!open_raw(path, &database)) {
        sqlite3_close(database);
        return false;
    }
    const bool success = exec_sql(database, kUpgradeV1ToV2Sql);
    sqlite3_close(database);
    return success;
}

bool scalar_integer(const std::filesystem::path& path, const char* sql,
                    sqlite3_int64& value) {
    sqlite3* database = nullptr;
    if (!open_raw(path, &database)) {
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
    if (!open_raw(path, &database)) {
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
        value.assign(text, static_cast<std::size_t>(
                               sqlite3_column_bytes(statement, 0)));
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return read;
}

bool create_header_only_database(const std::filesystem::path& path,
                                 int application_id, int user_version,
                                 const char* extra_sql = nullptr) {
    sqlite3* database = nullptr;
    if (!open_raw(path, &database)) {
        sqlite3_close(database);
        return false;
    }
    const std::string pragmas =
        "PRAGMA application_id=" + std::to_string(application_id) +
        ";PRAGMA user_version=" + std::to_string(user_version) + ";";
    bool success = exec_sql(database, pragmas.c_str());
    if (success && extra_sql != nullptr) {
        success = exec_sql(database, extra_sql);
    }
    sqlite3_close(database);
    return success;
}

bool mutate_sql(const std::filesystem::path& path, const char* sql) {
    sqlite3* database = nullptr;
    if (!open_raw(path, &database)) {
        sqlite3_close(database);
        return false;
    }
    const bool success = exec_sql(database, sql);
    sqlite3_close(database);
    return success;
}

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
}

bool no_sidecars_exist(const std::filesystem::path& path) {
    std::error_code error;
    return !std::filesystem::exists(path.string() + "-wal", error) &&
           !std::filesystem::exists(path.string() + "-shm", error) &&
           !std::filesystem::exists(path.string() + "-journal", error);
}

class RenameFailpoint final : public ISqliteHistoryFailpoint {
public:
    bool should_fail(SqliteHistoryWritePoint point) noexcept override {
        ++call_count;
        return point == SqliteHistoryWritePoint::MigrationPostsRenamed;
    }

    std::size_t call_count = 0;
};

class SeenTableFailpoint final : public ISqliteHistoryFailpoint {
public:
    bool should_fail(SqliteHistoryWritePoint point) noexcept override {
        ++call_count;
        return point ==
               SqliteHistoryWritePoint::MigrationSeenTableCreated;
    }

    std::size_t call_count = 0;
};

bool create_noncanonical_v1_fixture(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    if (!open_raw(path, &database)) {
        sqlite3_close(database);
        return false;
    }
    std::string loose_schema(kV1Schema);
    const std::string old_limit = "BETWEEN 1 AND 160";
    const auto limit_position = loose_schema.find(old_limit);
    if (limit_position == std::string::npos) {
        sqlite3_close(database);
        return false;
    }
    loose_schema.replace(limit_position, old_limit.size(),
                         "BETWEEN 1 AND 1000");
    const bool success =
        exec_sql(database, loose_schema.c_str()) &&
        exec_sql(
            database,
            "INSERT INTO meta(key,value) VALUES"
            " ('install_id',X'3031323334354637B8393A3B3C3D3E3F'),"
            " ('last_assigned_order',X'0000000000000001');"
            "INSERT INTO posts("
            " message_id,sender_id,sender_sequence,sender_user_id,body,"
            " reply_to,received_order,origin,local_state"
            ") VALUES("
            " X'101112131415461798191A1B1C1D1E1F',"
            " X'3031323334354637B8393A3B3C3D3E3F',"
            " X'0000000000000001','mika',"
            " substr(replace(hex(zeroblob(161)),'0','x'),1,161),"
            " NULL,X'0000000000000001',1,0"
            ");");
    sqlite3_close(database);
    return success;
}

bool create_v2_fixture_with_schema_change(const std::filesystem::path& path,
                                          const char* sql) {
    if (!create_v2_fixture(path)) {
        return false;
    }
    return mutate_sql(path, sql);
}

} // namespace

int main() {
    lora::test::Runner runner;

    runner.run("v1 fixture migrates transactionally without losing rows", [&] {
        TemporaryDatabase temporary("v1");
        auto identity = make_identity();
        auto expected = make_local_entry();
        REQUIRE(create_v1_fixture(temporary.path(), identity, expected));

        auto opened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(opened);
        auto loaded = opened.value().load_snapshot(identity);
        REQUIRE(loaded);
        CHECK_EQ(loaded.value().last_assigned_order, expected.received_order);
        REQUIRE(loaded.value().entries.size() == 1);
        CHECK_EQ(loaded.value().entries.front().post, expected.post);
        CHECK_EQ(loaded.value().entries.front().received_order,
                 expected.received_order);
        CHECK_EQ(loaded.value().entries.front().origin, expected.origin);
        CHECK(!loaded.value().entries.front().post.sender_time());

        sqlite3_int64 integer_value = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;",
                             integer_value));
        CHECK_EQ(integer_value, 3);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM pragma_table_info('posts')"
            " WHERE name='sender_time';",
            integer_value));
        CHECK_EQ(integer_value, 1);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM pragma_table_list"
            " WHERE name='seen_messages' AND strict=1;",
            integer_value));
        CHECK_EQ(integer_value, 1);
        CHECK_EQ(loaded.value().seen_messages.size(), 1U);
        CHECK_EQ(loaded.value().last_seen_order, 1U);
        std::string dedupe_initialized;
        CHECK(scalar_text(
            temporary.path(),
            "SELECT hex(value) FROM meta"
            " WHERE key='dedupe_initialized';",
            dedupe_initialized));
        CHECK_EQ(dedupe_initialized, "01");
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM pragma_table_list"
            " WHERE name='mentions' AND strict=1;",
            integer_value));
        CHECK_EQ(integer_value, 1);
    });

    runner.run("wrong application id is rejected without changing journal", [&] {
        TemporaryDatabase temporary("wrong-application");
        REQUIRE(create_header_only_database(
            temporary.path(), 0x12345678, 2,
            "CREATE TABLE foreign_data(value TEXT);"));
        auto opened = SqliteHistoryStore::open(temporary.path());
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::WrongApplication);

        sqlite3_int64 application_id = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA application_id;",
                             application_id));
        CHECK_EQ(application_id, 0x12345678);
        std::string journal_mode;
        CHECK(scalar_text(temporary.path(), "PRAGMA journal_mode;",
                          journal_mode));
        CHECK_EQ(journal_mode, "delete");
    });

    runner.run("failed v1 migration rolls back schema and version", [&] {
        TemporaryDatabase temporary("migration-rollback");
        auto identity = make_identity();
        auto entry = make_local_entry();
        REQUIRE(create_v1_fixture(temporary.path(), identity, entry));
        const auto original = read_bytes(temporary.path());
        REQUIRE(!original.empty());

        RenameFailpoint failpoint;
        auto opened =
            SqliteHistoryStore::open(temporary.path(), &failpoint);
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::MigrationFailed);
        CHECK_EQ(failpoint.call_count, 1U);

        sqlite3_int64 value = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;", value));
        CHECK_EQ(value, 1);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM sqlite_schema"
            " WHERE type='table' AND name='posts';",
            value));
        CHECK_EQ(value, 1);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM sqlite_schema"
            " WHERE type='table' AND name='posts_v1';",
            value));
        CHECK_EQ(value, 0);
        CHECK_EQ(read_bytes(temporary.path()), original);
        CHECK(no_sidecars_exist(temporary.path()));
    });

    runner.run("failed v2 dedupe migration rolls back schema and version",
               [&] {
        TemporaryDatabase temporary("dedupe-migration-rollback");
        REQUIRE(create_v2_fixture(temporary.path()));
        const auto original = read_bytes(temporary.path());
        REQUIRE(!original.empty());

        SeenTableFailpoint failpoint;
        auto opened =
            SqliteHistoryStore::open(temporary.path(), &failpoint);
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::MigrationFailed);
        CHECK_EQ(failpoint.call_count, 1U);

        sqlite3_int64 value = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;", value));
        CHECK_EQ(value, 2);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM sqlite_schema"
            " WHERE type='table' AND name='seen_messages';",
            value));
        CHECK_EQ(value, 0);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM pragma_table_info('meta')"
            " WHERE name='last_seen_order';",
            value));
        CHECK_EQ(value, 0);
        CHECK(scalar_integer(
            temporary.path(),
            "SELECT count(*) FROM meta"
            " WHERE key='dedupe_initialized';",
            value));
        CHECK_EQ(value, 0);
        CHECK_EQ(read_bytes(temporary.path()), original);
        CHECK(no_sidecars_exist(temporary.path()));
    });

    runner.run("noncanonical v1 schema is rejected without mutation", [&] {
        TemporaryDatabase temporary("noncanonical-v1");
        REQUIRE(create_noncanonical_v1_fixture(temporary.path()));
        const auto original = read_bytes(temporary.path());
        REQUIRE(!original.empty());

        auto opened = SqliteHistoryStore::open(temporary.path());
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::InvalidData);
        CHECK_EQ(read_bytes(temporary.path()), original);
        CHECK(no_sidecars_exist(temporary.path()));

        sqlite3_int64 value = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;", value));
        CHECK_EQ(value, 1);
        std::string journal_mode;
        CHECK(scalar_text(temporary.path(), "PRAGMA journal_mode;",
                          journal_mode));
        CHECK_EQ(journal_mode, "delete");
    });

    runner.run("v2 extra objects and columns are rejected without mutation",
               [&] {
        struct SchemaChange {
            const char* name;
            const char* sql;
        };
        constexpr std::array<SchemaChange, 4> changes{{
            {"trigger",
             "CREATE TRIGGER injected_trigger AFTER INSERT ON posts"
             " BEGIN UPDATE meta SET value=value; END;"},
            {"wildcard-prefix-trigger",
             "CREATE TRIGGER sqlitex_rewrite AFTER INSERT ON posts"
             " BEGIN UPDATE posts SET body='rewritten'"
             " WHERE message_id=NEW.message_id; END;"},
            {"reserved-prefix-trigger",
             "PRAGMA writable_schema=ON;"
             "INSERT INTO sqlite_schema(type,name,tbl_name,rootpage,sql)"
             " VALUES('trigger','sqlite_evil','posts',0,"
             "'CREATE TRIGGER sqlite_evil AFTER INSERT ON posts"
             " BEGIN UPDATE posts SET body=''rewritten''"
             " WHERE message_id=NEW.message_id; END');"
             "PRAGMA writable_schema=OFF;"},
            {"column", "ALTER TABLE posts ADD COLUMN injected TEXT;"},
        }};

        for (const auto& change : changes) {
            TemporaryDatabase temporary(
                std::string("noncanonical-v2-") + change.name);
            REQUIRE(create_v2_fixture_with_schema_change(
                temporary.path(), change.sql));
            const auto original = read_bytes(temporary.path());
            REQUIRE(!original.empty());
            const bool had_wal =
                std::filesystem::exists(temporary.path().string() + "-wal");
            const bool had_shm =
                std::filesystem::exists(temporary.path().string() + "-shm");
            const bool had_journal = std::filesystem::exists(
                temporary.path().string() + "-journal");

            auto opened = SqliteHistoryStore::open(temporary.path());
            CHECK(!opened);
            CHECK_EQ(opened.error(), HistoryError::InvalidData);
            CHECK_EQ(read_bytes(temporary.path()), original);
            CHECK_EQ(std::filesystem::exists(
                         temporary.path().string() + "-wal"),
                     had_wal);
            CHECK_EQ(std::filesystem::exists(
                         temporary.path().string() + "-shm"),
                     had_shm);
            CHECK_EQ(std::filesystem::exists(
                         temporary.path().string() + "-journal"),
                     had_journal);
        }
    });

    runner.run("unclaimed database with foreign tables is rejected", [&] {
        TemporaryDatabase temporary("unclaimed");
        REQUIRE(create_header_only_database(
            temporary.path(), 0, 0,
            "CREATE TABLE foreign_data(value TEXT);"));
        auto opened = SqliteHistoryStore::open(temporary.path());
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::WrongApplication);
    });

    runner.run("future schema version is never downgraded", [&] {
        TemporaryDatabase temporary("future");
        REQUIRE(create_header_only_database(
            temporary.path(),
            static_cast<int>(
                lora::adapters::storage::kHistoryApplicationId),
            99));
        auto opened = SqliteHistoryStore::open(temporary.path());
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::UnsupportedSchema);

        sqlite3_int64 version = 0;
        CHECK(scalar_integer(temporary.path(), "PRAGMA user_version;",
                             version));
        CHECK_EQ(version, 99);
    });

    runner.run("non-SQLite content is reported as corruption", [&] {
        TemporaryDatabase temporary("corrupt");
        {
            std::ofstream output(temporary.path(), std::ios::binary);
            output << "this is not a sqlite database";
        }
        auto opened = SqliteHistoryStore::open(temporary.path());
        CHECK(!opened);
        CHECK_EQ(opened.error(), HistoryError::CorruptDatabase);
    });

    runner.run("semantically invalid local history is rejected", [&] {
        TemporaryDatabase temporary("semantic");
        auto identity = make_identity(1);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            HistorySnapshot snapshot;
            snapshot.last_assigned_order = 1;
            snapshot.entries.push_back(make_local_entry(1, 1));
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
        }

        REQUIRE(mutate_sql(
            temporary.path(),
            "UPDATE posts"
            " SET sender_sequence=X'0000000000000002';"));
        auto reopened = SqliteHistoryStore::open(temporary.path());
        REQUIRE(reopened);
        auto loaded = reopened.value().load_snapshot(identity);
        CHECK(!loaded);
        CHECK_EQ(loaded.error(), HistoryError::InvalidData);
    });

    runner.run("foreign-key damage is rejected during open", [&] {
        TemporaryDatabase temporary("foreign-key");
        auto identity = make_identity(1);
        {
            auto opened = SqliteHistoryStore::open(temporary.path());
            REQUIRE(opened);
            auto post = lora::test::make_post(
                0x10, 0x30, 1, "with mention",
                {lora::test::make_install_id(0x40)}, std::nullopt,
                std::nullopt, "mika");
            HistorySnapshot snapshot;
            snapshot.last_assigned_order = 1;
            snapshot.entries.push_back(TimelineEntry{
                std::move(post), 1,
                LocalDelivery{LocalDeliveryState::Queued}});
            REQUIRE(opened.value().save_snapshot(snapshot, identity) ==
                    HistoryError::None);
        }

        REQUIRE(mutate_sql(
            temporary.path(),
            "PRAGMA foreign_keys=OFF; DELETE FROM posts;"));
        auto reopened = SqliteHistoryStore::open(temporary.path());
        CHECK(!reopened);
        CHECK_EQ(reopened.error(), HistoryError::InvalidData);
    });

    return runner.finish();
}
