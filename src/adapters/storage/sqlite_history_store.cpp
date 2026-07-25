/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/sqlite_history_store.h"

#include "core/limits.h"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lora::adapters::storage {
namespace {

constexpr int kBusyTimeoutMilliseconds = 5'000;

constexpr const char* kCreateMetaV2Sql = R"sql(
CREATE TABLE meta (
    key TEXT PRIMARY KEY NOT NULL
        CHECK(key IN ('install_id', 'last_assigned_order')),
    value BLOB NOT NULL,
    CHECK(
        (key = 'install_id' AND length(value) = 16) OR
        (key = 'last_assigned_order' AND length(value) = 8)
    )
) WITHOUT ROWID, STRICT;
)sql";

constexpr const char* kCreateMetaSql = R"sql(
CREATE TABLE meta (
    key TEXT PRIMARY KEY NOT NULL
        CHECK(key IN (
            'install_id', 'last_assigned_order', 'last_seen_order',
            'dedupe_initialized'
        )),
    value BLOB NOT NULL,
    CHECK(
        (key = 'install_id' AND length(value) = 16) OR
        (key IN ('last_assigned_order', 'last_seen_order')
         AND length(value) = 8) OR
        (key = 'dedupe_initialized' AND
         value IN (X'00', X'01'))
    )
) WITHOUT ROWID, STRICT;
)sql";

constexpr const char* kCreatePostsV1Sql = R"sql(
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

constexpr const char* kCreatePostsSql = R"sql(
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
)sql";

constexpr const char* kCreateMentionsSql = R"sql(
CREATE TABLE mentions (
    message_id BLOB NOT NULL,
    ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 3),
    mentioned_install_id BLOB NOT NULL
        CHECK(length(mentioned_install_id) = 16),
    PRIMARY KEY(message_id, ordinal),
    UNIQUE(message_id, mentioned_install_id),
    FOREIGN KEY(message_id) REFERENCES posts(message_id) ON DELETE CASCADE
) WITHOUT ROWID, STRICT;
)sql";

constexpr const char* kCreateSeenMessagesSql = R"sql(
CREATE TABLE seen_messages (
    message_id BLOB PRIMARY KEY NOT NULL
        CHECK(length(message_id) = 16),
    protocol_version INTEGER NOT NULL
        CHECK(protocol_version = 1),
    encoded_post BLOB NOT NULL
        CHECK(length(encoded_post) BETWEEN 46 AND 316),
    seen_order BLOB NOT NULL UNIQUE
        CHECK(length(seen_order) = 8
              AND seen_order <> X'0000000000000000')
) WITHOUT ROWID, STRICT;
)sql";

constexpr const char* kCreateSchemaSql = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE meta (
    key TEXT PRIMARY KEY NOT NULL
        CHECK(key IN (
            'install_id', 'last_assigned_order', 'last_seen_order',
            'dedupe_initialized'
        )),
    value BLOB NOT NULL,
    CHECK(
        (key = 'install_id' AND length(value) = 16) OR
        (key IN ('last_assigned_order', 'last_seen_order')
         AND length(value) = 8) OR
        (key = 'dedupe_initialized' AND
         value IN (X'00', X'01'))
    )
) WITHOUT ROWID, STRICT;
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
CREATE TABLE mentions (
    message_id BLOB NOT NULL,
    ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 3),
    mentioned_install_id BLOB NOT NULL
        CHECK(length(mentioned_install_id) = 16),
    PRIMARY KEY(message_id, ordinal),
    UNIQUE(message_id, mentioned_install_id),
    FOREIGN KEY(message_id) REFERENCES posts(message_id) ON DELETE CASCADE
) WITHOUT ROWID, STRICT;
CREATE TABLE seen_messages (
    message_id BLOB PRIMARY KEY NOT NULL
        CHECK(length(message_id) = 16),
    protocol_version INTEGER NOT NULL
        CHECK(protocol_version = 1),
    encoded_post BLOB NOT NULL
        CHECK(length(encoded_post) BETWEEN 46 AND 316),
    seen_order BLOB NOT NULL UNIQUE
        CHECK(length(seen_order) = 8
              AND seen_order <> X'0000000000000000')
) WITHOUT ROWID, STRICT;
PRAGMA application_id = 1280136007;
PRAGMA user_version = 3;
COMMIT;
)sql";

class Statement {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* statement) noexcept : statement_(statement) {}
    ~Statement() { sqlite3_finalize(statement_); }

    Statement(Statement&& other) noexcept
        : statement_(std::exchange(other.statement_, nullptr)) {}

    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            sqlite3_finalize(statement_);
            statement_ = std::exchange(other.statement_, nullptr);
        }
        return *this;
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database,
                         const char* begin_sql = "BEGIN IMMEDIATE;")
        : database_(database) {
        active_ = sqlite3_exec(database_, begin_sql, nullptr, nullptr, nullptr)
                  == SQLITE_OK;
    }

    ~Transaction() {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    bool started() const noexcept { return active_; }

    bool commit() noexcept {
        if (!active_) {
            return false;
        }
        if (sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* database_;
    bool active_ = false;
};

int primary_result_code(int result_code) noexcept {
    return result_code & 0xff;
}

bool is_corrupt_result(int result_code) noexcept {
    const int primary = primary_result_code(result_code);
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ||
           primary == SQLITE_FORMAT;
}

HistoryError error_for_result(int result_code, HistoryError fallback) noexcept {
    return is_corrupt_result(result_code) ? HistoryError::CorruptDatabase : fallback;
}

int exec(sqlite3* database, const char* sql) noexcept {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
}

int prepare(sqlite3* database, const char* sql, Statement& result) noexcept {
    sqlite3_stmt* raw = nullptr;
    const int status = sqlite3_prepare_v2(database, sql, -1, &raw, nullptr);
    if (status == SQLITE_OK) {
        result = Statement(raw);
    } else {
        sqlite3_finalize(raw);
    }
    return status;
}

bool query_integer(sqlite3* database, const char* sql, sqlite3_int64& value,
                   int& result_code) noexcept {
    Statement statement;
    result_code = prepare(database, sql, statement);
    if (result_code != SQLITE_OK) {
        return false;
    }
    result_code = sqlite3_step(statement.get());
    if (result_code != SQLITE_ROW ||
        sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        return false;
    }
    value = sqlite3_column_int64(statement.get(), 0);
    result_code = sqlite3_step(statement.get());
    return result_code == SQLITE_DONE;
}

bool query_text(sqlite3* database, const char* sql, std::string& value,
                int& result_code) {
    Statement statement;
    result_code = prepare(database, sql, statement);
    if (result_code != SQLITE_OK) {
        return false;
    }
    result_code = sqlite3_step(statement.get());
    if (result_code != SQLITE_ROW ||
        sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.get(), 0));
    const int byte_count = sqlite3_column_bytes(statement.get(), 0);
    if (bytes == nullptr || byte_count < 0) {
        return false;
    }
    value.assign(bytes, static_cast<std::size_t>(byte_count));
    result_code = sqlite3_step(statement.get());
    return result_code == SQLITE_DONE;
}

bool is_sql_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

std::string canonical_schema_sql(std::string_view sql) {
    std::string canonical;
    canonical.reserve(sql.size());
    char quote = '\0';
    for (std::size_t index = 0; index < sql.size(); ++index) {
        const char value = sql[index];
        if (quote != '\0') {
            canonical.push_back(value);
            if (value == quote) {
                if (index + 1U < sql.size() && sql[index + 1U] == quote) {
                    canonical.push_back(sql[++index]);
                } else {
                    quote = '\0';
                }
            }
            continue;
        }
        if (value == '\'' || value == '"' || value == '`') {
            quote = value;
            canonical.push_back(value);
            continue;
        }
        if (is_sql_space(value) || value == ';') {
            continue;
        }
        canonical.push_back(value);
    }
    return canonical;
}

bool sqlite_text_equals(sqlite3_stmt* statement, int column,
                        std::string_view expected) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return false;
    }
    const auto* text = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, column));
    const int byte_count = sqlite3_column_bytes(statement, column);
    return text != nullptr && byte_count >= 0 &&
           std::string_view(text, static_cast<std::size_t>(byte_count)) ==
               expected;
}

HistoryError verify_exact_schema(sqlite3* database, int schema_version) {
    Statement statement;
    int result_code = prepare(
        database,
        "SELECT type, name, tbl_name, sql FROM sqlite_schema"
        " ORDER BY type, name;",
        statement);
    if (result_code != SQLITE_OK) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }

    bool saw_meta = false;
    bool saw_posts = false;
    bool saw_mentions = false;
    bool saw_seen_messages = false;
    bool saw_posts_primary_index = false;
    bool saw_posts_order_index = false;
    bool saw_mentions_unique_index = false;
    bool saw_seen_order_index = false;
    std::size_t object_count = 0;
    while ((result_code = sqlite3_step(statement.get())) == SQLITE_ROW) {
        ++object_count;
        if (sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT ||
            (sqlite3_column_type(statement.get(), 3) != SQLITE_TEXT &&
             sqlite3_column_type(statement.get(), 3) != SQLITE_NULL)) {
            return HistoryError::InvalidData;
        }

        const auto* name_bytes = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 1));
        const auto* table_bytes = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 2));
        const auto* sql_bytes = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 3));
        const int name_size = sqlite3_column_bytes(statement.get(), 1);
        const int table_size = sqlite3_column_bytes(statement.get(), 2);
        const int sql_size = sqlite3_column_bytes(statement.get(), 3);
        if (name_bytes == nullptr || table_bytes == nullptr ||
            name_size <= 0 || table_size <= 0) {
            return HistoryError::InvalidData;
        }

        const std::string_view name(
            name_bytes, static_cast<std::size_t>(name_size));
        const std::string_view table(
            table_bytes, static_cast<std::size_t>(table_size));
        if (sqlite_text_equals(statement.get(), 0, "index")) {
            if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL ||
                sql_size != 0) {
                return HistoryError::InvalidData;
            }
            if (name == "sqlite_autoindex_posts_1" && table == "posts" &&
                !saw_posts_primary_index) {
                saw_posts_primary_index = true;
            } else if (name == "sqlite_autoindex_posts_2" &&
                       table == "posts" && !saw_posts_order_index) {
                saw_posts_order_index = true;
            } else if (schema_version >= 2 &&
                       name == "sqlite_autoindex_mentions_2" &&
                       table == "mentions" &&
                       !saw_mentions_unique_index) {
                saw_mentions_unique_index = true;
            } else if (schema_version == 3 &&
                       name == "sqlite_autoindex_seen_messages_2" &&
                       table == "seen_messages" &&
                       !saw_seen_order_index) {
                saw_seen_order_index = true;
            } else {
                return HistoryError::InvalidData;
            }
            continue;
        }
        if (!sqlite_text_equals(statement.get(), 0, "table") ||
            sql_bytes == nullptr || sql_size <= 0 || name != table) {
            return HistoryError::InvalidData;
        }

        const std::string stored = canonical_schema_sql(std::string_view(
            sql_bytes, static_cast<std::size_t>(sql_size)));
        if (name == "meta" && !saw_meta) {
            saw_meta = true;
            const auto* expected =
                schema_version == 3 ? kCreateMetaSql
                                    : kCreateMetaV2Sql;
            if (stored != canonical_schema_sql(expected)) {
                return HistoryError::InvalidData;
            }
        } else if (name == "posts" && !saw_posts) {
            saw_posts = true;
            const auto* expected =
                schema_version == 1 ? kCreatePostsV1Sql : kCreatePostsSql;
            if (stored != canonical_schema_sql(expected)) {
                return HistoryError::InvalidData;
            }
        } else if (name == "mentions" && schema_version >= 2 &&
                   !saw_mentions) {
            saw_mentions = true;
            if (stored != canonical_schema_sql(kCreateMentionsSql)) {
                return HistoryError::InvalidData;
            }
        } else if (name == "seen_messages" &&
                   schema_version == 3 && !saw_seen_messages) {
            saw_seen_messages = true;
            if (stored !=
                canonical_schema_sql(kCreateSeenMessagesSql)) {
                return HistoryError::InvalidData;
            }
        } else {
            return HistoryError::InvalidData;
        }
    }
    if (result_code != SQLITE_DONE) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }

    const std::size_t expected_count =
        schema_version == 1 ? 4U : (schema_version == 2 ? 6U : 8U);
    return object_count == expected_count && saw_meta && saw_posts &&
                   saw_posts_primary_index && saw_posts_order_index &&
                   (schema_version == 1 ||
                    (saw_mentions && saw_mentions_unique_index)) &&
                   (schema_version != 3 ||
                    (saw_seen_messages && saw_seen_order_index))
               ? HistoryError::None
               : HistoryError::InvalidData;
}

std::array<std::uint8_t, 8> encode_uint64(std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = encoded.size(); index > 0; --index) {
        encoded[index - 1] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
    return encoded;
}

bool decode_uint64(sqlite3_stmt* statement, int column,
                   std::uint64_t& value) noexcept {
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB ||
        sqlite3_column_bytes(statement, column) != 8) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(
        sqlite3_column_blob(statement, column));
    if (bytes == nullptr) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[index];
    }
    return true;
}

bool read_uuid(sqlite3_stmt* statement, int column, core::Uuid& value) noexcept {
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB ||
        sqlite3_column_bytes(statement, column) != 16) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(
        sqlite3_column_blob(statement, column));
    if (bytes == nullptr) {
        return false;
    }
    core::Uuid::Bytes raw{};
    std::copy_n(bytes, raw.size(), raw.begin());
    value = core::Uuid::from_bytes(raw);
    return true;
}

bool read_text(sqlite3_stmt* statement, int column, std::size_t maximum_bytes,
               std::string& value) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return false;
    }
    const int byte_count = sqlite3_column_bytes(statement, column);
    const auto* text = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, column));
    if (text == nullptr || byte_count <= 0 ||
        static_cast<std::size_t>(byte_count) > maximum_bytes ||
        std::memchr(text, '\0', static_cast<std::size_t>(byte_count)) != nullptr) {
        return false;
    }
    value.assign(text, static_cast<std::size_t>(byte_count));
    return true;
}

int bind_blob(sqlite3_stmt* statement, int parameter,
              const void* data, int byte_count) noexcept {
    return sqlite3_bind_blob(statement, parameter, data, byte_count,
                             SQLITE_TRANSIENT);
}

int bind_uuid(sqlite3_stmt* statement, int parameter,
              const core::Uuid& uuid) noexcept {
    const auto& bytes = uuid.bytes();
    return bind_blob(statement, parameter, bytes.data(),
                     static_cast<int>(bytes.size()));
}

int bind_uint64(sqlite3_stmt* statement, int parameter,
                std::uint64_t value) noexcept {
    const auto encoded = encode_uint64(value);
    return bind_blob(statement, parameter, encoded.data(),
                     static_cast<int>(encoded.size()));
}

bool statement_completed(sqlite3_stmt* statement) noexcept {
    return sqlite3_step(statement) == SQLITE_DONE;
}

bool reset_statement(sqlite3_stmt* statement) noexcept {
    return sqlite3_reset(statement) == SQLITE_OK &&
           sqlite3_clear_bindings(statement) == SQLITE_OK;
}

HistoryError run_integrity_checks(sqlite3* database) {
    std::string integrity;
    int result_code = SQLITE_OK;
    if (!query_text(database, "PRAGMA integrity_check(1);", integrity, result_code)) {
        return error_for_result(result_code, HistoryError::ReadFailed);
    }
    if (integrity != "ok") {
        return HistoryError::CorruptDatabase;
    }

    Statement foreign_keys;
    result_code = prepare(database, "PRAGMA foreign_key_check;", foreign_keys);
    if (result_code != SQLITE_OK) {
        return error_for_result(result_code, HistoryError::ReadFailed);
    }
    result_code = sqlite3_step(foreign_keys.get());
    if (result_code == SQLITE_ROW) {
        return HistoryError::InvalidData;
    }
    return result_code == SQLITE_DONE
               ? HistoryError::None
               : error_for_result(result_code, HistoryError::ReadFailed);
}

HistoryError verify_v2_or_v3_schema(
    sqlite3* database, int schema_version) {
    if (schema_version != 2 && schema_version != 3) {
        return HistoryError::InvalidData;
    }
    const HistoryError exact =
        verify_exact_schema(database, schema_version);
    if (exact != HistoryError::None) {
        return exact;
    }

    constexpr const char* kProbeSql =
        "SELECT m.key, m.value FROM meta AS m LIMIT 0;"
        "SELECT p.message_id, p.sender_id, p.sender_sequence, p.sender_user_id,"
        " p.body, p.reply_to, p.sender_time, p.received_order, p.origin,"
        " p.local_state FROM posts AS p LIMIT 0;"
        "SELECT n.message_id, n.ordinal, n.mentioned_install_id"
        " FROM mentions AS n LIMIT 0;";

    const char* cursor = kProbeSql;
    while (*cursor != '\0') {
        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        const int result_code =
            sqlite3_prepare_v2(database, cursor, -1, &raw, &tail);
        Statement statement(raw);
        if (result_code != SQLITE_OK || tail == cursor) {
            return error_for_result(result_code, HistoryError::InvalidData);
        }
        cursor = tail;
    }
    if (schema_version == 3) {
        Statement seen_messages;
        const int result_code = prepare(
            database,
            "SELECT s.message_id, s.protocol_version, s.encoded_post,"
            " s.seen_order FROM seen_messages AS s LIMIT 0;",
            seen_messages);
        if (result_code != SQLITE_OK) {
            return error_for_result(
                result_code, HistoryError::InvalidData);
        }
    }

    Statement strict_tables;
    const char* strict_sql =
        schema_version == 3
            ? "SELECT name, strict FROM pragma_table_list"
              " WHERE schema = 'main' AND name IN"
              " ('meta', 'posts', 'mentions', 'seen_messages')"
              " ORDER BY name;"
            : "SELECT name, strict FROM pragma_table_list"
              " WHERE schema = 'main' AND name IN"
              " ('meta', 'posts', 'mentions')"
              " ORDER BY name;";
    int result_code = prepare(
        database, strict_sql, strict_tables);
    if (result_code != SQLITE_OK) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }

    std::size_t table_count = 0;
    while ((result_code = sqlite3_step(strict_tables.get())) == SQLITE_ROW) {
        if (sqlite3_column_type(strict_tables.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(strict_tables.get(), 1) != SQLITE_INTEGER ||
            sqlite3_column_int(strict_tables.get(), 1) != 1) {
            return HistoryError::InvalidData;
        }
        ++table_count;
    }
    if (result_code != SQLITE_DONE) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }
    const std::size_t expected_table_count =
        schema_version == 3 ? 4U : 3U;
    if (table_count != expected_table_count) {
        return HistoryError::InvalidData;
    }

    sqlite3_int64 all_table_count = 0;
    if (!query_integer(
            database,
            "SELECT count(*) FROM sqlite_schema"
            " WHERE type='table';",
            all_table_count, result_code)) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }
    return all_table_count ==
                   static_cast<sqlite3_int64>(expected_table_count)
               ? HistoryError::None
               : HistoryError::InvalidData;
}

HistoryError verify_v2_schema(sqlite3* database) {
    return verify_v2_or_v3_schema(database, 2);
}

HistoryError verify_v3_schema(sqlite3* database) {
    return verify_v2_or_v3_schema(database, 3);
}

HistoryError verify_v1_schema(sqlite3* database) {
    const HistoryError exact = verify_exact_schema(database, 1);
    if (exact != HistoryError::None) {
        return exact;
    }

    constexpr const char* kProbeSql =
        "SELECT m.key, m.value FROM meta AS m LIMIT 0;"
        "SELECT p.message_id, p.sender_id, p.sender_sequence, p.sender_user_id,"
        " p.body, p.reply_to, p.received_order, p.origin, p.local_state"
        " FROM posts AS p LIMIT 0;";

    const char* cursor = kProbeSql;
    while (*cursor != '\0') {
        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        const int result_code =
            sqlite3_prepare_v2(database, cursor, -1, &raw, &tail);
        Statement statement(raw);
        if (result_code != SQLITE_OK || tail == cursor) {
            return error_for_result(result_code, HistoryError::InvalidData);
        }
        cursor = tail;
    }

    sqlite3_int64 strict_table_count = 0;
    sqlite3_int64 all_table_count = 0;
    int result_code = SQLITE_OK;
    if (!query_integer(
            database,
            "SELECT count(*) FROM pragma_table_list"
            " WHERE schema='main' AND name IN ('meta','posts') AND strict=1;",
            strict_table_count, result_code) ||
        !query_integer(
            database,
            "SELECT count(*) FROM sqlite_schema"
            " WHERE type='table';",
            all_table_count, result_code)) {
        return error_for_result(result_code, HistoryError::InvalidData);
    }
    return strict_table_count == 2 && all_table_count == 2
               ? HistoryError::None
               : HistoryError::InvalidData;
}

HistoryError migrate_v1_to_v2(sqlite3* database,
                              ISqliteHistoryFailpoint* failpoint) {
    Transaction transaction(database);
    if (!transaction.started()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }

    if (exec(database, "ALTER TABLE posts RENAME TO posts_v1;") != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }
    if (failpoint != nullptr &&
        failpoint->should_fail(
            SqliteHistoryWritePoint::MigrationPostsRenamed)) {
        return HistoryError::MigrationFailed;
    }
    if (exec(database, kCreatePostsSql) != SQLITE_OK ||
        exec(database,
             "INSERT INTO posts("
             " message_id, sender_id, sender_sequence, sender_user_id, body,"
             " reply_to, sender_time, received_order, origin, local_state"
             ") SELECT message_id, sender_id, sender_sequence, sender_user_id,"
             " body, reply_to, NULL, received_order, origin, local_state"
             " FROM posts_v1;") != SQLITE_OK ||
        exec(database, "DROP TABLE posts_v1;") != SQLITE_OK ||
        exec(database, kCreateMentionsSql) != SQLITE_OK ||
        exec(database, "PRAGMA user_version = 2;") != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }

    if (!transaction.commit()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }
    return HistoryError::None;
}

HistoryError migrate_v2_to_v3(
    sqlite3* database, ISqliteHistoryFailpoint* failpoint) {
    Transaction transaction(database);
    if (!transaction.started()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }

    if (exec(database, "ALTER TABLE meta RENAME TO meta_v2;") != SQLITE_OK ||
        exec(database, kCreateMetaSql) != SQLITE_OK ||
        exec(database,
             "INSERT INTO meta(key, value)"
             " SELECT key, value FROM meta_v2;") != SQLITE_OK ||
        exec(database,
             "INSERT INTO meta(key, value)"
             " VALUES('last_seen_order', X'0000000000000000');") !=
            SQLITE_OK ||
        exec(database,
             "INSERT INTO meta(key, value)"
             " VALUES('dedupe_initialized', X'00');") !=
            SQLITE_OK ||
        exec(database, "DROP TABLE meta_v2;") != SQLITE_OK ||
        exec(database, kCreateSeenMessagesSql) != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }
    if (failpoint != nullptr &&
        failpoint->should_fail(
            SqliteHistoryWritePoint::MigrationSeenTableCreated)) {
        return HistoryError::MigrationFailed;
    }
    if (exec(database, "PRAGMA user_version = 3;") != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }

    if (!transaction.commit()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::MigrationFailed);
    }
    return HistoryError::None;
}

HistoryError configure_connection(sqlite3* database) {
    sqlite3_extended_result_codes(database, 1);
    if (sqlite3_busy_timeout(database, kBusyTimeoutMilliseconds) != SQLITE_OK) {
        return HistoryError::ConfigureFailed;
    }

    int enabled = 0;
    if (sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled)
            != SQLITE_OK ||
        enabled != 1 ||
        exec(database, "PRAGMA trusted_schema = OFF;") != SQLITE_OK ||
        exec(database, "PRAGMA foreign_keys = ON;") != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::ConfigureFailed);
    }
    return HistoryError::None;
}

HistoryError read_database_header(sqlite3* database,
                                  sqlite3_int64& application_id,
                                  sqlite3_int64& user_version,
                                  sqlite3_int64& table_count) {
    int result_code = SQLITE_OK;
    if (!query_integer(database, "PRAGMA application_id;", application_id,
                       result_code) ||
        !query_integer(database, "PRAGMA user_version;", user_version,
                       result_code) ||
        !query_integer(database,
                       "SELECT count(*) FROM sqlite_schema;",
                       table_count, result_code)) {
        return error_for_result(result_code, HistoryError::ConfigureFailed);
    }
    return HistoryError::None;
}

HistoryError validate_existing_database(sqlite3* database) {
    sqlite3_int64 application_id = 0;
    sqlite3_int64 user_version = 0;
    sqlite3_int64 table_count = 0;
    const HistoryError header = read_database_header(
        database, application_id, user_version, table_count);
    if (header != HistoryError::None) {
        return header;
    }
    const bool is_new_database =
        application_id == 0 && user_version == 0 && table_count == 0;
    if (is_new_database ||
        application_id != static_cast<sqlite3_int64>(kHistoryApplicationId)) {
        return HistoryError::WrongApplication;
    }
    if (user_version < 1 || user_version > kHistorySchemaVersion) {
        return HistoryError::UnsupportedSchema;
    }

    const HistoryError integrity = run_integrity_checks(database);
    if (integrity != HistoryError::None) {
        return integrity;
    }
    if (user_version == 1) {
        return verify_v1_schema(database);
    }
    return user_version == 2 ? verify_v2_schema(database)
                             : verify_v3_schema(database);
}

HistoryError configure_database(sqlite3* database,
                                ISqliteHistoryFailpoint* failpoint,
                                bool enable_wal = true) {
    const HistoryError connection = configure_connection(database);
    if (connection != HistoryError::None) {
        return connection;
    }

    sqlite3_int64 application_id = 0;
    sqlite3_int64 user_version = 0;
    sqlite3_int64 table_count = 0;
    const HistoryError header = read_database_header(
        database, application_id, user_version, table_count);
    if (header != HistoryError::None) {
        return header;
    }
    const bool is_new_database =
        application_id == 0 && user_version == 0 && table_count == 0;

    if (is_new_database) {
        if (exec(database, kCreateSchemaSql) != SQLITE_OK) {
            exec(database, "ROLLBACK;");
            return error_for_result(sqlite3_extended_errcode(database),
                                    HistoryError::WriteFailed);
        }
    } else {
        const HistoryError existing = validate_existing_database(database);
        if (existing != HistoryError::None) {
            return existing;
        }
    }
    if (!is_new_database && user_version == 1) {
        const HistoryError migration =
            migrate_v1_to_v2(database, failpoint);
        if (migration != HistoryError::None) {
            return migration;
        }
        user_version = 2;
    }
    if (!is_new_database && user_version == 2) {
        const HistoryError migration =
            migrate_v2_to_v3(database, failpoint);
        if (migration != HistoryError::None) {
            return migration;
        }
        user_version = 3;
    }

    const HistoryError schema = verify_v3_schema(database);
    if (schema != HistoryError::None) {
        return schema;
    }
    const HistoryError integrity = run_integrity_checks(database);
    if (integrity != HistoryError::None) {
        return integrity;
    }
    if (!enable_wal) {
        return HistoryError::None;
    }

    int result_code = SQLITE_OK;
    std::string journal_mode;
    if (!query_text(database, "PRAGMA journal_mode = WAL;", journal_mode,
                    result_code) ||
        journal_mode != "wal" ||
        exec(database, "PRAGMA synchronous = FULL;") != SQLITE_OK) {
        return error_for_result(result_code, HistoryError::ConfigureFailed);
    }

    sqlite3_int64 foreign_keys = 0;
    sqlite3_int64 synchronous = 0;
    sqlite3_int64 trusted_schema = 1;
    if (!query_integer(database, "PRAGMA foreign_keys;", foreign_keys,
                       result_code) ||
        !query_integer(database, "PRAGMA synchronous;", synchronous,
                       result_code) ||
        !query_integer(database, "PRAGMA trusted_schema;", trusted_schema,
                       result_code) ||
        foreign_keys != 1 || synchronous != 2 || trusted_schema != 0) {
        return error_for_result(result_code, HistoryError::ConfigureFailed);
    }
    return HistoryError::None;
}

HistoryError operation_preflight(sqlite3* database) {
    sqlite3_int64 application_id = 0;
    sqlite3_int64 user_version = 0;
    int result_code = SQLITE_OK;
    if (!query_integer(database, "PRAGMA application_id;", application_id,
                       result_code) ||
        !query_integer(database, "PRAGMA user_version;", user_version,
                       result_code)) {
        return error_for_result(result_code, HistoryError::ReadFailed);
    }
    if (application_id != static_cast<sqlite3_int64>(kHistoryApplicationId)) {
        return HistoryError::WrongApplication;
    }
    if (user_version != kHistorySchemaVersion) {
        return HistoryError::UnsupportedSchema;
    }
    const HistoryError schema = verify_v3_schema(database);
    if (schema != HistoryError::None) {
        return schema;
    }
    return run_integrity_checks(database);
}

bool restrict_database_permissions(
    const std::filesystem::path& database_path) noexcept {
    const std::array<std::filesystem::path, 3> files{
        database_path,
        std::filesystem::path(database_path.string() + "-wal"),
        std::filesystem::path(database_path.string() + "-shm"),
    };
    constexpr auto kPrivatePermissions =
        std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write;

    for (const auto& file : files) {
        std::error_code error;
        const bool exists = std::filesystem::exists(file, error);
        if (error) {
            return false;
        }
        if (!exists) {
            continue;
        }
        std::filesystem::permissions(
            file, kPrivatePermissions,
            std::filesystem::perm_options::replace, error);
        if (error) {
            return false;
        }
    }
    return true;
}

HistoryError inspect_regular_leaf(const std::filesystem::path& path,
                                  bool& exists) noexcept {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        exists = false;
        return errno == ENOENT ? HistoryError::None
                               : HistoryError::OpenFailed;
    }
    exists = true;
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        status.st_uid != ::geteuid()) {
        return HistoryError::InvalidData;
    }
    return HistoryError::None;
}

core::Result<bool, HistoryError> inspect_history_artifacts(
    const std::filesystem::path& database_path) {
    bool database_exists = false;
    HistoryError error =
        inspect_regular_leaf(database_path, database_exists);
    if (error != HistoryError::None) {
        return core::Result<bool, HistoryError>::failure(error);
    }

    for (const auto& path : {
             std::filesystem::path(database_path.string() + "-wal"),
             std::filesystem::path(database_path.string() + "-shm"),
             std::filesystem::path(database_path.string() + "-journal")}) {
        bool exists = false;
        error = inspect_regular_leaf(path, exists);
        if (error != HistoryError::None) {
            return core::Result<bool, HistoryError>::failure(error);
        }
        if (exists && !database_exists) {
            return core::Result<bool, HistoryError>::failure(
                HistoryError::InvalidData);
        }
    }
    return core::Result<bool, HistoryError>::success(database_exists);
}

int read_leaf_flags() noexcept {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    return flags;
}

int create_leaf_flags() noexcept {
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

void close_descriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        while (::close(descriptor) != 0 && errno == EINTR) {
        }
    }
}

HistoryError copy_regular_leaf(const std::filesystem::path& source,
                               const std::filesystem::path& destination,
                               bool required) {
    const int source_descriptor =
        ::open(source.c_str(), read_leaf_flags());
    if (source_descriptor < 0) {
        if (!required && errno == ENOENT) {
            return HistoryError::None;
        }
        return HistoryError::OpenFailed;
    }

    struct stat source_status {};
    if (::fstat(source_descriptor, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode) ||
        source_status.st_nlink != 1 ||
        source_status.st_uid != ::geteuid()) {
        close_descriptor(source_descriptor);
        return HistoryError::InvalidData;
    }

    const int destination_descriptor =
        ::open(destination.c_str(), create_leaf_flags(),
               S_IRUSR | S_IWUSR);
    if (destination_descriptor < 0) {
        close_descriptor(source_descriptor);
        return HistoryError::OpenFailed;
    }

    HistoryError result = HistoryError::None;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (result == HistoryError::None) {
        const ssize_t bytes_read =
            ::read(source_descriptor, buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = HistoryError::ReadFailed;
            break;
        }

        const std::size_t byte_count =
            static_cast<std::size_t>(bytes_read);
        std::size_t written = 0;
        while (written < byte_count) {
            const ssize_t bytes_written =
                ::write(destination_descriptor,
                        buffer.data() + written,
                        byte_count - written);
            if (bytes_written < 0 && errno == EINTR) {
                continue;
            }
            if (bytes_written <= 0) {
                result = HistoryError::ReadFailed;
                break;
            }
            written += static_cast<std::size_t>(bytes_written);
        }
    }

    if (::fchmod(destination_descriptor, S_IRUSR | S_IWUSR) != 0 &&
        result == HistoryError::None) {
        result = HistoryError::OpenFailed;
    }
    close_descriptor(destination_descriptor);
    close_descriptor(source_descriptor);
    return result;
}

core::Result<std::filesystem::path, HistoryError>
make_history_probe_copy(const std::filesystem::path& database_path) {
    std::error_code filesystem_error;
    const std::filesystem::path probe_directory =
        history_probe_directory_path(database_path);
    const bool created =
        std::filesystem::create_directory(probe_directory, filesystem_error);
    if (filesystem_error || !created) {
        return core::Result<std::filesystem::path, HistoryError>::failure(
            HistoryError::OpenFailed);
    }
    const std::filesystem::path probe_database =
        probe_directory / database_path.filename();
    HistoryError result = HistoryError::None;
    if (::chmod(probe_directory.c_str(), S_IRWXU) != 0) {
        result = HistoryError::OpenFailed;
    }
    if (result == HistoryError::None) {
        result = copy_regular_leaf(database_path, probe_database, true);
    }
    for (const std::string_view suffix : {"-wal", "-journal"}) {
        if (result != HistoryError::None) {
            break;
        }
        result = copy_regular_leaf(
            std::filesystem::path(database_path.string() +
                                  std::string(suffix)),
            std::filesystem::path(probe_database.string() +
                                  std::string(suffix)),
            false);
    }
    if (result != HistoryError::None) {
        static_cast<void>(discard_history_probe(database_path));
        return core::Result<std::filesystem::path, HistoryError>::failure(
            result);
    }
    return core::Result<std::filesystem::path, HistoryError>::success(
        probe_database);
}

core::Result<sqlite3*, HistoryError> open_probe_database(
    const std::filesystem::path& probe_database) {
    sqlite3* database = nullptr;
    constexpr int kProbeFlags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW;
    const int open_result = sqlite3_open_v2(
        probe_database.string().c_str(), &database, kProbeFlags, nullptr);
    if (open_result != SQLITE_OK) {
        const HistoryError open_error =
            error_for_result(open_result, HistoryError::OpenFailed);
        sqlite3_close_v2(database);
        return core::Result<sqlite3*, HistoryError>::failure(open_error);
    }
    return core::Result<sqlite3*, HistoryError>::success(database);
}

bool sync_parent_directory(
    const std::filesystem::path& directory_path) noexcept {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(directory_path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    while (::fsync(descriptor) != 0) {
        if (errno != EINTR) {
            close_descriptor(descriptor);
            return false;
        }
    }
    close_descriptor(descriptor);
    return true;
}

HistoryError discard_probe_directory_impl(
    const std::filesystem::path& database_path) {
    const std::filesystem::path probe_directory =
        history_probe_directory_path(database_path);
    struct stat directory_status {};
    if (::lstat(probe_directory.c_str(), &directory_status) != 0) {
        return errno == ENOENT ? HistoryError::None
                               : HistoryError::OpenFailed;
    }
    if (!S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid()) {
        return HistoryError::InvalidData;
    }

    const std::string database_name = database_path.filename().string();
    const std::set<std::string> allowed_names{
        database_name,
        database_name + "-wal",
        database_name + "-shm",
        database_name + "-journal",
    };
    std::vector<std::filesystem::path> leaves;
    std::error_code filesystem_error;
    std::filesystem::directory_iterator iterator(
        probe_directory, filesystem_error);
    const std::filesystem::directory_iterator end;
    while (!filesystem_error && iterator != end) {
        const std::string name = iterator->path().filename().string();
        if (allowed_names.find(name) == allowed_names.end()) {
            return HistoryError::InvalidData;
        }
        leaves.push_back(iterator->path());
        iterator.increment(filesystem_error);
    }
    if (filesystem_error) {
        return HistoryError::OpenFailed;
    }

    for (const auto& leaf : leaves) {
        while (::unlink(leaf.c_str()) != 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ENOENT) {
                return HistoryError::OpenFailed;
            }
            break;
        }
    }
    while (::rmdir(probe_directory.c_str()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno != ENOENT) {
            return HistoryError::OpenFailed;
        }
        break;
    }
    const bool synced = sync_parent_directory(database_path.parent_path());
    return synced
               ? HistoryError::None
               : HistoryError::OpenFailed;
}

HistoryError remove_probe_copy(const std::filesystem::path& database_path,
                               HistoryError result) {
    const HistoryError cleanup = discard_history_probe(database_path);
    return cleanup != HistoryError::None && result == HistoryError::None
               ? cleanup
               : result;
}

HistoryError preflight_existing_schema_read_only(
    const std::filesystem::path& database_path) {
    auto copied = make_history_probe_copy(database_path);
    if (!copied) {
        return copied.error();
    }
    auto opened = open_probe_database(copied.value());
    if (!opened) {
        return remove_probe_copy(database_path, opened.error());
    }
    sqlite3* database = opened.value();
    const HistoryError connection = configure_connection(database);
    const HistoryError result =
        connection == HistoryError::None
            ? validate_existing_database(database)
            : connection;
    if (sqlite3_close_v2(database) != SQLITE_OK &&
        result == HistoryError::None) {
        return remove_probe_copy(database_path, HistoryError::ReadFailed);
    }
    return remove_probe_copy(database_path, result);
}

struct StoredMetadata {
    bool initialized = false;
    bool dedupe_initialized = false;
    core::Uuid::Bytes install_id{};
    std::uint64_t last_assigned_order = 0;
    std::uint64_t last_seen_order = 0;
};

core::Result<StoredMetadata, HistoryError> load_metadata(sqlite3* database) {
    Statement statement;
    int result_code = prepare(database,
                              "SELECT key, value FROM meta ORDER BY key;",
                              statement);
    if (result_code != SQLITE_OK) {
        return core::Result<StoredMetadata, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    StoredMetadata metadata;
    bool saw_install_id = false;
    bool saw_last_order = false;
    bool saw_last_seen_order = false;
    bool saw_dedupe_initialized = false;
    std::size_t row_count = 0;
    while ((result_code = sqlite3_step(statement.get())) == SQLITE_ROW) {
        ++row_count;
        if (row_count > 4 ||
            sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(statement.get(), 1) != SQLITE_BLOB) {
            return core::Result<StoredMetadata, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        const auto* key_bytes = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 0));
        const int key_size = sqlite3_column_bytes(statement.get(), 0);
        if (key_bytes == nullptr || key_size <= 0) {
            return core::Result<StoredMetadata, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        const std::string_view key(key_bytes, static_cast<std::size_t>(key_size));
        if (key == "dedupe_initialized") {
            const auto* bytes = static_cast<const std::uint8_t*>(
                sqlite3_column_blob(statement.get(), 1));
            if (saw_dedupe_initialized ||
                sqlite3_column_bytes(statement.get(), 1) != 1 ||
                bytes == nullptr ||
                (bytes[0] != 0U && bytes[0] != 1U)) {
                return core::Result<StoredMetadata, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            metadata.dedupe_initialized = bytes[0] == 1U;
            saw_dedupe_initialized = true;
        } else if (key == "install_id") {
            if (saw_install_id ||
                sqlite3_column_bytes(statement.get(), 1) != 16) {
                return core::Result<StoredMetadata, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            const auto* bytes = static_cast<const std::uint8_t*>(
                sqlite3_column_blob(statement.get(), 1));
            if (bytes == nullptr) {
                return core::Result<StoredMetadata, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            std::copy_n(bytes, metadata.install_id.size(),
                        metadata.install_id.begin());
            saw_install_id = true;
        } else if (key == "last_assigned_order") {
            if (saw_last_order ||
                !decode_uint64(statement.get(), 1,
                               metadata.last_assigned_order)) {
                return core::Result<StoredMetadata, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            saw_last_order = true;
        } else if (key == "last_seen_order") {
            if (saw_last_seen_order ||
                !decode_uint64(statement.get(), 1,
                               metadata.last_seen_order)) {
                return core::Result<StoredMetadata, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            saw_last_seen_order = true;
        } else {
            return core::Result<StoredMetadata, HistoryError>::failure(
                HistoryError::InvalidData);
        }
    }
    if (result_code != SQLITE_DONE) {
        return core::Result<StoredMetadata, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    if (row_count == 0) {
        sqlite3_int64 post_count = 0;
        sqlite3_int64 seen_count = 0;
        if (!query_integer(database, "SELECT count(*) FROM posts;", post_count,
                           result_code) ||
            !query_integer(database,
                           "SELECT count(*) FROM seen_messages;", seen_count,
                           result_code)) {
            return core::Result<StoredMetadata, HistoryError>::failure(
                error_for_result(result_code, HistoryError::ReadFailed));
        }
        if (post_count != 0 || seen_count != 0) {
            return core::Result<StoredMetadata, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        return core::Result<StoredMetadata, HistoryError>::success(metadata);
    }
    if (!saw_dedupe_initialized || !saw_install_id ||
        !saw_last_order || !saw_last_seen_order ||
        row_count != 4) {
        return core::Result<StoredMetadata, HistoryError>::failure(
            HistoryError::InvalidData);
    }
    metadata.initialized = true;
    return core::Result<StoredMetadata, HistoryError>::success(metadata);
}

bool identity_matches(const StoredMetadata& metadata,
                      const model::Identity& identity) noexcept {
    return metadata.install_id == identity.install_id().uuid().bytes();
}

HistoryError validate_snapshot(const HistorySnapshot& snapshot,
                               const model::Identity& identity) {
    if (snapshot.entries.size() > core::kMaxTimelineEntries) {
        return HistoryError::InvalidSnapshot;
    }
    auto dedupe = protocol::MessageDedupeWindow::restore(
        protocol::kDefaultSeenMessageCapacity,
        snapshot.last_seen_order, snapshot.seen_messages);
    if (!dedupe) {
        return HistoryError::InvalidSnapshot;
    }

    std::set<core::MessageId> message_ids;
    std::set<std::uint64_t> received_orders;
    std::uint64_t maximum_order = 0;
    std::uint64_t previous_order = 0;
    std::uint64_t previous_local_sequence = 0;
    std::size_t queued_local_count = 0;
    for (const auto& entry : snapshot.entries) {
        if (entry.received_order == 0 ||
            entry.received_order <= previous_order ||
            !message_ids.insert(entry.post.message_id()).second ||
            !received_orders.insert(entry.received_order).second) {
            return HistoryError::InvalidSnapshot;
        }
        previous_order = entry.received_order;
        maximum_order = std::max(maximum_order, entry.received_order);

        const auto* local = std::get_if<model::LocalDelivery>(&entry.origin);
        if (local != nullptr &&
            (entry.post.sender_id() != identity.install_id() ||
             entry.post.sender_sequence() >
                 identity.last_issued_sender_sequence() ||
             entry.post.sender_sequence() <= previous_local_sequence)) {
                return HistoryError::InvalidSnapshot;
        }
        if (local != nullptr) {
            previous_local_sequence = entry.post.sender_sequence();
            if (local->state ==
                model::LocalDeliveryState::Queued) {
                ++queued_local_count;
                if (queued_local_count >
                    core::kMaxQueuedLocalPosts) {
                    return HistoryError::InvalidSnapshot;
                }
            }
        }

        auto encoded = protocol::encode_post(entry.post);
        if (!encoded ||
            dedupe.value().classify(entry.post.message_id(),
                                    encoded.value()) !=
                protocol::DedupeClassification::Duplicate) {
            return HistoryError::InvalidSnapshot;
        }
    }
    return maximum_order <= snapshot.last_assigned_order
               ? HistoryError::None
               : HistoryError::InvalidSnapshot;
}

HistoryError reconcile_snapshot_dedupe(HistorySnapshot& snapshot) {
    auto dedupe = protocol::MessageDedupeWindow::restore(
        protocol::kDefaultSeenMessageCapacity,
        snapshot.last_seen_order, std::move(snapshot.seen_messages));
    if (!dedupe) {
        return HistoryError::InvalidSnapshot;
    }

    std::vector<core::MessageId> protected_message_ids;
    protected_message_ids.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        protected_message_ids.push_back(entry.post.message_id());
    }
    for (const auto& entry : snapshot.entries) {
        auto encoded = protocol::encode_post(entry.post);
        if (!encoded) {
            return HistoryError::InvalidSnapshot;
        }
        const auto classification = dedupe.value().classify(
            entry.post.message_id(), encoded.value());
        if (classification == protocol::DedupeClassification::Conflict ||
            classification == protocol::DedupeClassification::Invalid) {
            return HistoryError::InvalidSnapshot;
        }
        if (classification == protocol::DedupeClassification::New &&
            dedupe.value().remember(
                entry.post, encoded.value(), protected_message_ids) !=
                protocol::DedupeError::None) {
            return HistoryError::InvalidSnapshot;
        }
    }

    snapshot.last_seen_order = dedupe.value().last_seen_order();
    snapshot.seen_messages = dedupe.value().records();
    return HistoryError::None;
}

core::Result<bool, HistoryError> ledger_matches_snapshot(
    sqlite3* database, const HistorySnapshot& snapshot) {
    Statement statement;
    int result_code = prepare(
        database,
        "SELECT message_id, encoded_post, seen_order"
        " FROM seen_messages ORDER BY seen_order;",
        statement);
    if (result_code != SQLITE_OK) {
        return core::Result<bool, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    std::size_t index = 0;
    while ((result_code = sqlite3_step(statement.get())) ==
           SQLITE_ROW) {
        if (index >= snapshot.seen_messages.size() ||
            sqlite3_column_type(statement.get(), 0) != SQLITE_BLOB ||
            sqlite3_column_bytes(statement.get(), 0) != 16 ||
            sqlite3_column_type(statement.get(), 1) != SQLITE_BLOB) {
            return core::Result<bool, HistoryError>::success(false);
        }
        const auto* message_id =
            static_cast<const std::uint8_t*>(
                sqlite3_column_blob(statement.get(), 0));
        const auto* encoded_post =
            static_cast<const std::uint8_t*>(
                sqlite3_column_blob(statement.get(), 1));
        const int encoded_size =
            sqlite3_column_bytes(statement.get(), 1);
        std::uint64_t seen_order = 0;
        const auto& expected = snapshot.seen_messages[index];
        const auto& expected_id = expected.message_id.uuid().bytes();
        if (message_id == nullptr || encoded_post == nullptr ||
            encoded_size < 0 ||
            static_cast<std::size_t>(encoded_size) !=
                expected.encoded_post.size() ||
            !decode_uint64(statement.get(), 2, seen_order) ||
            seen_order != expected.seen_order ||
            !std::equal(expected_id.begin(), expected_id.end(),
                        message_id) ||
            !std::equal(expected.encoded_post.begin(),
                        expected.encoded_post.end(),
                        encoded_post)) {
            return core::Result<bool, HistoryError>::success(false);
        }
        ++index;
    }
    if (result_code != SQLITE_DONE) {
        return core::Result<bool, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }
    return core::Result<bool, HistoryError>::success(
        index == snapshot.seen_messages.size());
}

HistoryError write_snapshot(
    sqlite3* database, const HistorySnapshot& snapshot,
    const model::Identity& identity,
    ISqliteHistoryFailpoint* failpoint) {
    Transaction transaction(database);
    if (!transaction.started()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }

    auto metadata = load_metadata(database);
    if (!metadata) {
        return metadata.error();
    }
    if (metadata.value().initialized &&
        !identity_matches(metadata.value(), identity)) {
        return HistoryError::IdentityMismatch;
    }
    if (metadata.value().initialized &&
        (snapshot.last_assigned_order <
             metadata.value().last_assigned_order ||
         snapshot.last_seen_order <
             metadata.value().last_seen_order)) {
        return HistoryError::InvalidSnapshot;
    }
    if (metadata.value().initialized &&
        snapshot.last_seen_order ==
            metadata.value().last_seen_order) {
        auto ledger_matches =
            ledger_matches_snapshot(database, snapshot);
        if (!ledger_matches) {
            return ledger_matches.error();
        }
        if (!ledger_matches.value()) {
            return HistoryError::InvalidSnapshot;
        }
    }

    if (exec(database, "DELETE FROM posts;") != SQLITE_OK ||
        exec(database, "DELETE FROM seen_messages;") != SQLITE_OK ||
        exec(database, "DELETE FROM meta;") != SQLITE_OK) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }

    Statement insert_meta;
    Statement insert_post;
    Statement insert_mention;
    Statement insert_seen_message;
    int result_code = prepare(
        database, "INSERT INTO meta(key, value) VALUES(?1, ?2);", insert_meta);
    if (result_code == SQLITE_OK) {
        result_code = prepare(
            database,
            "INSERT INTO posts("
            " message_id, sender_id, sender_sequence, sender_user_id, body,"
            " reply_to, sender_time, received_order, origin, local_state"
            ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);",
            insert_post);
    }
    if (result_code == SQLITE_OK) {
        result_code = prepare(
            database,
            "INSERT INTO mentions(message_id, ordinal, mentioned_install_id)"
            " VALUES(?1, ?2, ?3);",
            insert_mention);
    }
    if (result_code == SQLITE_OK) {
        result_code = prepare(
            database,
            "INSERT INTO seen_messages("
            " message_id, protocol_version, encoded_post, seen_order"
            ") VALUES(?1, 1, ?2, ?3);",
            insert_seen_message);
    }
    if (result_code != SQLITE_OK) {
        return error_for_result(result_code, HistoryError::WriteFailed);
    }

    const auto& install_bytes = identity.install_id().uuid().bytes();
    if (sqlite3_bind_text(insert_meta.get(), 1, "install_id", -1,
                          SQLITE_STATIC) != SQLITE_OK ||
        bind_blob(insert_meta.get(), 2, install_bytes.data(),
                  static_cast<int>(install_bytes.size())) != SQLITE_OK ||
        !statement_completed(insert_meta.get()) ||
        !reset_statement(insert_meta.get())) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }
    if (sqlite3_bind_text(insert_meta.get(), 1, "last_assigned_order", -1,
                          SQLITE_STATIC) != SQLITE_OK ||
        bind_uint64(insert_meta.get(), 2, snapshot.last_assigned_order) !=
            SQLITE_OK ||
        !statement_completed(insert_meta.get()) ||
        !reset_statement(insert_meta.get())) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }
    if (sqlite3_bind_text(insert_meta.get(), 1, "last_seen_order", -1,
                          SQLITE_STATIC) != SQLITE_OK ||
        bind_uint64(insert_meta.get(), 2, snapshot.last_seen_order) !=
            SQLITE_OK ||
        !statement_completed(insert_meta.get()) ||
        !reset_statement(insert_meta.get())) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }
    const std::uint8_t dedupe_initialized = 1U;
    if (sqlite3_bind_text(insert_meta.get(), 1, "dedupe_initialized", -1,
                          SQLITE_STATIC) != SQLITE_OK ||
        bind_blob(insert_meta.get(), 2, &dedupe_initialized, 1) !=
            SQLITE_OK ||
        !statement_completed(insert_meta.get())) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }

    for (const auto& entry : snapshot.entries) {
        const auto sequence = encode_uint64(entry.post.sender_sequence());
        const auto received_order = encode_uint64(entry.received_order);
        const auto& user_id = entry.post.sender_user_id().value();
        const auto& body = entry.post.body().value();

        if (bind_uuid(insert_post.get(), 1,
                      entry.post.message_id().uuid()) != SQLITE_OK ||
            bind_uuid(insert_post.get(), 2,
                      entry.post.sender_id().uuid()) != SQLITE_OK ||
            bind_blob(insert_post.get(), 3, sequence.data(),
                      static_cast<int>(sequence.size())) != SQLITE_OK ||
            sqlite3_bind_text(insert_post.get(), 4, user_id.data(),
                              static_cast<int>(user_id.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(insert_post.get(), 5, body.data(),
                              static_cast<int>(body.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            return HistoryError::WriteFailed;
        }

        if (entry.post.reply_to()) {
            if (bind_uuid(insert_post.get(), 6,
                          entry.post.reply_to()->uuid()) != SQLITE_OK) {
                return HistoryError::WriteFailed;
            }
        } else if (sqlite3_bind_null(insert_post.get(), 6) != SQLITE_OK) {
            return HistoryError::WriteFailed;
        }

        if (entry.post.sender_time()) {
            if (sqlite3_bind_int64(insert_post.get(), 7,
                                   *entry.post.sender_time()) != SQLITE_OK) {
                return HistoryError::WriteFailed;
            }
        } else if (sqlite3_bind_null(insert_post.get(), 7) != SQLITE_OK) {
            return HistoryError::WriteFailed;
        }

        int origin = 0;
        int local_state = 0;
        const auto* local = std::get_if<model::LocalDelivery>(&entry.origin);
        if (local != nullptr) {
            origin = 1;
            local_state = static_cast<int>(local->state);
        }
        if (bind_blob(insert_post.get(), 8, received_order.data(),
                      static_cast<int>(received_order.size())) != SQLITE_OK ||
            sqlite3_bind_int(insert_post.get(), 9, origin) != SQLITE_OK ||
            (local != nullptr
                 ? sqlite3_bind_int(insert_post.get(), 10, local_state)
                 : sqlite3_bind_null(insert_post.get(), 10)) != SQLITE_OK ||
            !statement_completed(insert_post.get()) ||
            !reset_statement(insert_post.get())) {
            return error_for_result(sqlite3_extended_errcode(database),
                                    HistoryError::WriteFailed);
        }

        const auto& mentions = entry.post.mentions();
        for (std::size_t index = 0; index < mentions.size(); ++index) {
            if (bind_uuid(insert_mention.get(), 1,
                          entry.post.message_id().uuid()) != SQLITE_OK ||
                sqlite3_bind_int(insert_mention.get(), 2,
                                 static_cast<int>(index)) != SQLITE_OK ||
                bind_uuid(insert_mention.get(), 3,
                          mentions[index].uuid()) != SQLITE_OK ||
                !statement_completed(insert_mention.get()) ||
                !reset_statement(insert_mention.get())) {
                return error_for_result(sqlite3_extended_errcode(database),
                                        HistoryError::WriteFailed);
            }
        }
    }

    if (failpoint != nullptr &&
        failpoint->should_fail(
            SqliteHistoryWritePoint::SnapshotTimelineWritten)) {
        return HistoryError::WriteFailed;
    }
    for (const auto& record : snapshot.seen_messages) {
        if (bind_uuid(insert_seen_message.get(), 1,
                      record.message_id.uuid()) != SQLITE_OK ||
            bind_blob(insert_seen_message.get(), 2,
                      record.encoded_post.data(),
                      static_cast<int>(record.encoded_post.size())) !=
                SQLITE_OK ||
            bind_uint64(insert_seen_message.get(), 3,
                        record.seen_order) != SQLITE_OK ||
            !statement_completed(insert_seen_message.get()) ||
            !reset_statement(insert_seen_message.get())) {
            return error_for_result(sqlite3_extended_errcode(database),
                                    HistoryError::WriteFailed);
        }
    }
    if (failpoint != nullptr &&
        failpoint->should_fail(
            SqliteHistoryWritePoint::SnapshotDedupeWritten)) {
        return HistoryError::WriteFailed;
    }

    if (!transaction.commit()) {
        return error_for_result(sqlite3_extended_errcode(database),
                                HistoryError::WriteFailed);
    }
    return HistoryError::None;
}

core::Result<HistorySnapshot, HistoryError> read_snapshot(
    sqlite3* database, const StoredMetadata& metadata,
    const model::Identity& identity) {
    Statement posts;
    int result_code = prepare(
        database,
        "SELECT message_id, sender_id, sender_sequence, sender_user_id, body,"
        " reply_to, sender_time, received_order, origin, local_state"
        " FROM posts ORDER BY received_order;",
        posts);
    if (result_code != SQLITE_OK) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    Statement mentions;
    result_code = prepare(
        database,
        "SELECT ordinal, mentioned_install_id FROM mentions"
        " WHERE message_id = ?1 ORDER BY ordinal;",
        mentions);
    if (result_code != SQLITE_OK) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    HistorySnapshot snapshot;
    snapshot.last_assigned_order = metadata.last_assigned_order;
    snapshot.last_seen_order = metadata.last_seen_order;
    snapshot.entries.reserve(core::kMaxTimelineEntries);
    snapshot.seen_messages.reserve(
        protocol::kDefaultSeenMessageCapacity);
    std::set<std::uint64_t> seen_orders;
    std::uint64_t previous_local_sequence = 0;

    while ((result_code = sqlite3_step(posts.get())) == SQLITE_ROW) {
        if (snapshot.entries.size() >= core::kMaxTimelineEntries) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        core::Uuid message_id;
        core::Uuid sender_id;
        std::uint64_t sender_sequence = 0;
        std::uint64_t received_order = 0;
        std::string sender_user_id;
        std::string body;
        if (!read_uuid(posts.get(), 0, message_id) ||
            !read_uuid(posts.get(), 1, sender_id) ||
            !decode_uint64(posts.get(), 2, sender_sequence) ||
            sender_sequence == 0 ||
            !read_text(posts.get(), 3, core::kMaxUserIdBytes,
                       sender_user_id) ||
            !read_text(posts.get(), 4, core::kMaxPostBodyBytes, body) ||
            !decode_uint64(posts.get(), 7, received_order) ||
            received_order == 0 ||
            !seen_orders.insert(received_order).second ||
            received_order > metadata.last_assigned_order) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        std::optional<core::Uuid> reply_to;
        if (sqlite3_column_type(posts.get(), 5) == SQLITE_BLOB) {
            core::Uuid reply;
            if (!read_uuid(posts.get(), 5, reply)) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            reply_to = reply;
        } else if (sqlite3_column_type(posts.get(), 5) != SQLITE_NULL) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        std::optional<ports::UnixSeconds> sender_time;
        if (sqlite3_column_type(posts.get(), 6) == SQLITE_INTEGER) {
            sender_time = sqlite3_column_int64(posts.get(), 6);
        } else if (sqlite3_column_type(posts.get(), 6) != SQLITE_NULL) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        if (bind_uuid(mentions.get(), 1, message_id) != SQLITE_OK) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::ReadFailed);
        }
        std::vector<core::Uuid> mention_ids;
        int mention_result = SQLITE_OK;
        while ((mention_result = sqlite3_step(mentions.get())) == SQLITE_ROW) {
            if (mention_ids.size() >= core::kMaxMentions ||
                sqlite3_column_type(mentions.get(), 0) != SQLITE_INTEGER ||
                sqlite3_column_int64(mentions.get(), 0) !=
                    static_cast<sqlite3_int64>(mention_ids.size())) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            core::Uuid mention;
            if (!read_uuid(mentions.get(), 1, mention)) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            mention_ids.push_back(mention);
        }
        if (mention_result != SQLITE_DONE || !reset_statement(mentions.get())) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                error_for_result(mention_result, HistoryError::ReadFailed));
        }

        model::PostPayloadInput input;
        input.message_id = message_id;
        input.sender_id = sender_id;
        input.sender_sequence = sender_sequence;
        input.sender_user_id = std::move(sender_user_id);
        input.body = std::move(body);
        input.mentions = std::move(mention_ids);
        input.reply_to = reply_to;
        input.sender_time = sender_time;
        auto post = model::PostPayload::create(std::move(input));
        if (!post) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        model::EntryOrigin origin;
        if (sqlite3_column_type(posts.get(), 8) != SQLITE_INTEGER) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        const int origin_value = sqlite3_column_int(posts.get(), 8);
        if (origin_value == 0) {
            if (sqlite3_column_type(posts.get(), 9) != SQLITE_NULL) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            origin = model::ReceivedOrigin{};
        } else if (origin_value == 1) {
            if (sqlite3_column_type(posts.get(), 9) != SQLITE_INTEGER) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            const int local_state = sqlite3_column_int(posts.get(), 9);
            if (local_state < static_cast<int>(model::LocalDeliveryState::Queued) ||
                local_state > static_cast<int>(model::LocalDeliveryState::Unknown) ||
                post.value().sender_id() != identity.install_id() ||
                post.value().sender_sequence() >
                    identity.last_issued_sender_sequence() ||
                post.value().sender_sequence() <= previous_local_sequence) {
                return core::Result<HistorySnapshot, HistoryError>::failure(
                    HistoryError::InvalidData);
            }
            previous_local_sequence = post.value().sender_sequence();
            origin = model::LocalDelivery{
                static_cast<model::LocalDeliveryState>(local_state)};
        } else {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        snapshot.entries.push_back(model::TimelineEntry{
            std::move(post).value(), received_order, std::move(origin)});
    }
    if (result_code != SQLITE_DONE) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    Statement seen_messages;
    result_code = prepare(
        database,
        "SELECT message_id, protocol_version, encoded_post, seen_order"
        " FROM seen_messages ORDER BY seen_order;",
        seen_messages);
    if (result_code != SQLITE_OK) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }
    std::uint64_t previous_seen_order = 0;
    while ((result_code = sqlite3_step(seen_messages.get())) == SQLITE_ROW) {
        if (snapshot.seen_messages.size() >=
                protocol::kDefaultSeenMessageCapacity ||
            sqlite3_column_type(seen_messages.get(), 1) != SQLITE_INTEGER ||
            sqlite3_column_int64(seen_messages.get(), 1) != 1 ||
            sqlite3_column_type(seen_messages.get(), 2) != SQLITE_BLOB) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        core::Uuid raw_message_id;
        std::uint64_t seen_order = 0;
        const int encoded_size =
            sqlite3_column_bytes(seen_messages.get(), 2);
        const auto* encoded_data =
            static_cast<const std::uint8_t*>(
                sqlite3_column_blob(seen_messages.get(), 2));
        if (!read_uuid(seen_messages.get(), 0, raw_message_id) ||
            encoded_size <
                static_cast<int>(protocol::kMinimumEncodedPostBytes) ||
            encoded_size >
                static_cast<int>(protocol::kMaximumEncodedPostBytes) ||
            encoded_data == nullptr ||
            !decode_uint64(seen_messages.get(), 3, seen_order) ||
            seen_order == 0 || seen_order <= previous_seen_order ||
            seen_order > metadata.last_seen_order) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        auto message_id =
            core::MessageId::from_uuid(std::move(raw_message_id));
        if (!message_id) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }

        protocol::Bytes encoded_post(
            encoded_data, encoded_data + encoded_size);
        auto decoded = protocol::decode_post(encoded_post);
        auto canonical =
            decoded ? protocol::encode_post(decoded.value())
                    : core::Result<protocol::Bytes,
                                   protocol::PostCodecError>::failure(
                          protocol::PostCodecError::InvalidPost);
        if (!decoded || !canonical ||
            decoded.value().message_id() != message_id.value() ||
            canonical.value() != encoded_post) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        previous_seen_order = seen_order;
        snapshot.seen_messages.push_back(protocol::SeenMessageRecord{
            std::move(message_id).value(), seen_order,
            std::move(encoded_post)});
    }
    if (result_code != SQLITE_DONE) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(result_code, HistoryError::ReadFailed));
    }

    if (!metadata.dedupe_initialized) {
        if (!snapshot.seen_messages.empty() ||
            snapshot.last_seen_order != 0) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }
        auto seeded = snapshot;
        if (reconcile_snapshot_dedupe(seeded) !=
                HistoryError::None ||
            validate_snapshot(seeded, identity) !=
                HistoryError::None) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                HistoryError::InvalidData);
        }
    } else if (validate_snapshot(snapshot, identity) !=
               HistoryError::None) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            HistoryError::InvalidData);
    }
    return core::Result<HistorySnapshot, HistoryError>::success(
        std::move(snapshot));
}

HistoryError validate_bound_database_read_only(
    const std::filesystem::path& database_path,
    const model::Identity& identity) {
    auto copied = make_history_probe_copy(database_path);
    if (!copied) {
        return copied.error();
    }
    auto opened = open_probe_database(copied.value());
    if (!opened) {
        return remove_probe_copy(database_path, opened.error());
    }
    sqlite3* probe = opened.value();

    HistoryError result = configure_database(probe, nullptr, false);
    if (result == HistoryError::None) {
        auto metadata = load_metadata(probe);
        if (!metadata) {
            result = metadata.error();
        } else if (!metadata.value().initialized) {
            result = HistoryError::InvalidData;
        } else if (!identity_matches(metadata.value(), identity)) {
            result = HistoryError::IdentityMismatch;
        } else {
            auto snapshot =
                read_snapshot(probe, metadata.value(), identity);
            if (!snapshot) {
                result = snapshot.error();
            }
        }
    }

    if (sqlite3_close_v2(probe) != SQLITE_OK &&
        result == HistoryError::None) {
        result = HistoryError::ReadFailed;
    }
    return remove_probe_copy(database_path, result);
}

} // namespace

std::filesystem::path history_probe_directory_path(
    const std::filesystem::path& database_path) {
    return std::filesystem::path(database_path.string() + ".probe");
}

HistoryError discard_history_probe(
    const std::filesystem::path& database_path) {
    if (!database_path.is_absolute() || database_path.filename().empty()) {
        return HistoryError::InvalidPath;
    }
    return discard_probe_directory_impl(database_path);
}

SqliteHistoryStore::SqliteHistoryStore(
    sqlite3* database, std::filesystem::path database_path,
    ISqliteHistoryFailpoint* failpoint)
    : database_(database),
      database_path_(std::move(database_path)),
      failpoint_(failpoint) {}

SqliteHistoryStore::SqliteHistoryStore(SqliteHistoryStore&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      database_path_(std::move(other.database_path_)),
      failpoint_(std::exchange(other.failpoint_, nullptr)) {}

SqliteHistoryStore& SqliteHistoryStore::operator=(
    SqliteHistoryStore&& other) noexcept {
    if (this != &other) {
        sqlite3_close_v2(database_);
        database_ = std::exchange(other.database_, nullptr);
        database_path_ = std::move(other.database_path_);
        failpoint_ = std::exchange(other.failpoint_, nullptr);
    }
    return *this;
}

SqliteHistoryStore::~SqliteHistoryStore() {
    sqlite3_close_v2(database_);
}

core::Result<SqliteHistoryStore, HistoryError> SqliteHistoryStore::open(
    const std::filesystem::path& database_path,
    ISqliteHistoryFailpoint* failpoint) {
    if (!database_path.is_absolute() || database_path.filename().empty()) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::InvalidPath);
    }

    std::error_code filesystem_error;
    const auto parent = database_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return core::Result<SqliteHistoryStore, HistoryError>::failure(
                HistoryError::OpenFailed);
        }
    }

    const std::string path = database_path.string();
    if (path.empty() || path.find('\0') != std::string::npos) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::InvalidPath);
    }

    auto artifacts = inspect_history_artifacts(database_path);
    if (!artifacts) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            artifacts.error());
    }
    if (artifacts.value()) {
        const HistoryError preflight =
            preflight_existing_schema_read_only(database_path);
        if (preflight != HistoryError::None) {
            return core::Result<SqliteHistoryStore, HistoryError>::failure(
                preflight);
        }
    }

    sqlite3* database = nullptr;
    const int create_flag =
        artifacts.value() ? 0 : SQLITE_OPEN_CREATE;
    const int open_flags =
        SQLITE_OPEN_READWRITE | create_flag | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW;
    const int open_result =
        sqlite3_open_v2(path.c_str(), &database, open_flags, nullptr);
    if (open_result != SQLITE_OK) {
        const HistoryError error =
            error_for_result(open_result, HistoryError::OpenFailed);
        sqlite3_close_v2(database);
        return core::Result<SqliteHistoryStore, HistoryError>::failure(error);
    }

    SqliteHistoryStore store(database, database_path, failpoint);
    const HistoryError configure_result =
        configure_database(database, failpoint);
    if (configure_result != HistoryError::None) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            configure_result);
    }
    if (!restrict_database_permissions(database_path)) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::OpenFailed);
    }
    return core::Result<SqliteHistoryStore, HistoryError>::success(
        std::move(store));
}

core::Result<SqliteHistoryStore, HistoryError>
SqliteHistoryStore::open_existing_bound(
    const std::filesystem::path& database_path,
    const model::Identity& identity) {
    if (!database_path.is_absolute() || database_path.filename().empty()) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::InvalidPath);
    }
    const std::string path = database_path.string();
    if (path.empty() || path.find('\0') != std::string::npos) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::InvalidPath);
    }

    auto artifacts = inspect_history_artifacts(database_path);
    if (!artifacts) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            artifacts.error());
    }
    if (!artifacts.value()) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::OpenFailed);
    }

    const HistoryError validation =
        validate_bound_database_read_only(database_path, identity);
    if (validation != HistoryError::None) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            validation);
    }

    sqlite3* database = nullptr;
    constexpr int kOpenFlags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW;
    const int open_result =
        sqlite3_open_v2(path.c_str(), &database, kOpenFlags, nullptr);
    if (open_result != SQLITE_OK) {
        const HistoryError error =
            error_for_result(open_result, HistoryError::OpenFailed);
        sqlite3_close_v2(database);
        return core::Result<SqliteHistoryStore, HistoryError>::failure(error);
    }

    SqliteHistoryStore store(database, database_path, nullptr);
    const HistoryError configure_result =
        configure_database(database, nullptr);
    if (configure_result != HistoryError::None) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            configure_result);
    }
    if (!restrict_database_permissions(database_path)) {
        return core::Result<SqliteHistoryStore, HistoryError>::failure(
            HistoryError::OpenFailed);
    }
    return core::Result<SqliteHistoryStore, HistoryError>::success(
        std::move(store));
}

core::Result<HistorySnapshot, HistoryError> SqliteHistoryStore::load_snapshot(
    const model::Identity& identity) {
    Transaction transaction(database_, "BEGIN;");
    if (!transaction.started()) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(sqlite3_extended_errcode(database_),
                             HistoryError::ReadFailed));
    }
    const HistoryError preflight = operation_preflight(database_);
    if (preflight != HistoryError::None) {
        return core::Result<HistorySnapshot, HistoryError>::failure(preflight);
    }

    auto metadata = load_metadata(database_);
    if (!metadata) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            metadata.error());
    }
    if (!metadata.value().initialized) {
        if (!transaction.commit()) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                error_for_result(sqlite3_extended_errcode(database_),
                                 HistoryError::ReadFailed));
        }
        return core::Result<HistorySnapshot, HistoryError>::success(
            HistorySnapshot{});
    }
    if (!identity_matches(metadata.value(), identity)) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            HistoryError::IdentityMismatch);
    }
    auto snapshot = read_snapshot(database_, metadata.value(), identity);
    if (!snapshot) {
        return snapshot;
    }
    const bool migration_seed_pending =
        !metadata.value().dedupe_initialized;
    if (!transaction.commit()) {
        return core::Result<HistorySnapshot, HistoryError>::failure(
            error_for_result(sqlite3_extended_errcode(database_),
                             HistoryError::ReadFailed));
    }
    if (migration_seed_pending) {
        auto migrated = std::move(snapshot).value();
        const HistoryError reconciled =
            reconcile_snapshot_dedupe(migrated);
        if (reconciled != HistoryError::None) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                reconciled);
        }
        const HistoryError saved = save_snapshot(migrated, identity);
        if (saved != HistoryError::None) {
            return core::Result<HistorySnapshot, HistoryError>::failure(
                saved);
        }
        return core::Result<HistorySnapshot, HistoryError>::success(
            std::move(migrated));
    }
    return snapshot;
}

HistoryError SqliteHistoryStore::save_snapshot(
    const HistorySnapshot& snapshot, const model::Identity& identity) {
    HistorySnapshot candidate = snapshot;
    const HistoryError reconciled =
        reconcile_snapshot_dedupe(candidate);
    if (reconciled != HistoryError::None) {
        return reconciled;
    }
    const HistoryError validation = validate_snapshot(candidate, identity);
    if (validation != HistoryError::None) {
        return validation;
    }
    const HistoryError preflight = operation_preflight(database_);
    if (preflight != HistoryError::None) {
        return preflight;
    }

    return write_snapshot(
        database_, candidate, identity, failpoint_);
}

HistoryError SqliteHistoryStore::save_snapshot(
    const model::Timeline& timeline, const model::Identity& identity) {
    auto loaded = load_snapshot(identity);
    if (!loaded) {
        return loaded.error();
    }
    auto snapshot = std::move(loaded).value();
    snapshot.entries = timeline.entries();
    snapshot.last_assigned_order = timeline.last_assigned_order();
    return save_snapshot(snapshot, identity);
}

const std::filesystem::path& SqliteHistoryStore::database_path() const noexcept {
    return database_path_;
}

} // namespace lora::adapters::storage
