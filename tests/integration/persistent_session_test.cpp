/*
 * SPDX-License-Identifier: MIT
 */

#include "persistent_session.h"

#include "adapters/storage/sqlite_history_store.h"
#include "sqlite3.h"

#include "../unit/test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::array<char, 48U> pattern{};
        const std::string value = "/tmp/lora-session-test-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            root_ = std::filesystem::canonical(created);
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path& root() const noexcept { return root_; }
    std::filesystem::path config() const { return root_ / "config"; }
    std::filesystem::path data() const { return root_ / "data"; }
    std::filesystem::path settings() const {
        return config() / "lora-messenger" / "settings.json";
    }
    std::filesystem::path database() const {
        return data() / "lora-messenger" / "history.sqlite3";
    }

private:
    std::filesystem::path root_;
};

class EnvironmentGuard {
public:
    EnvironmentGuard(const std::filesystem::path& config,
                     const std::filesystem::path& data)
        : old_config_(read("XDG_CONFIG_HOME")),
          old_data_(read("XDG_DATA_HOME")),
          old_home_(read("HOME")) {
        static_cast<void>(::setenv(
            "XDG_CONFIG_HOME", config.c_str(), 1));
        static_cast<void>(::setenv(
            "XDG_DATA_HOME", data.c_str(), 1));
        static_cast<void>(::setenv("HOME", "/nonexistent", 1));
    }

    ~EnvironmentGuard() {
        restore("XDG_CONFIG_HOME", old_config_);
        restore("XDG_DATA_HOME", old_data_);
        restore("HOME", old_home_);
    }

private:
    static std::optional<std::string> read(const char* name) {
        const char* value = std::getenv(name);
        return value ? std::optional<std::string>{value} : std::nullopt;
    }

    static void restore(
        const char* name, const std::optional<std::string>& value) {
        if (value) {
            static_cast<void>(::setenv(name, value->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name));
        }
    }

    std::optional<std::string> old_config_;
    std::optional<std::string> old_data_;
    std::optional<std::string> old_home_;
};

void write_file(const std::filesystem::path& path,
                std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool scalar_integer(const std::filesystem::path& path,
                    const char* sql, sqlite3_int64& value) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            path.string().c_str(), &database,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
            nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(
            database, sql, -1, &statement, nullptr) == SQLITE_OK;
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

bool scalar_text(const std::filesystem::path& path,
                 const char* sql, std::string& value) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            path.string().c_str(), &database,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW,
            nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(
            database, sql, -1, &statement, nullptr) == SQLITE_OK;
    const bool read =
        prepared && sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) == SQLITE_TEXT;
    if (read) {
        const auto* text = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        const int size = sqlite3_column_bytes(statement, 0);
        if (text == nullptr || size < 0) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            return false;
        }
        value.assign(text, static_cast<std::size_t>(size));
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return read;
}

lora::core::Uuid test_uuid(std::uint8_t seed) {
    lora::core::Uuid::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            seed + static_cast<std::uint8_t>(index));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return lora::core::Uuid::from_bytes(bytes);
}

lora::core::Uuid indexed_uuid(std::uint32_t index,
                              std::uint8_t domain) {
    lora::core::Uuid::Bytes bytes{};
    bytes[0] = domain;
    bytes[1] = static_cast<std::uint8_t>(index >> 24U);
    bytes[2] = static_cast<std::uint8_t>(index >> 16U);
    bytes[3] = static_cast<std::uint8_t>(index >> 8U);
    bytes[4] = static_cast<std::uint8_t>(index);
    for (std::size_t offset = 5U; offset < bytes.size(); ++offset) {
        bytes[offset] = static_cast<std::uint8_t>(
            domain + static_cast<std::uint8_t>(offset));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return lora::core::Uuid::from_bytes(bytes);
}

lora::model::PostPayload indexed_received_post(std::uint32_t index) {
    lora::model::PostPayloadInput input;
    input.message_id = indexed_uuid(index, 0x31U);
    input.sender_id = indexed_uuid(0U, 0x71U);
    input.sender_sequence = static_cast<std::uint64_t>(index) + 1U;
    input.sender_user_id = "Peer";
    input.body = "post-" + std::to_string(index);
    auto post = lora::model::PostPayload::create(std::move(input));
    if (!post) {
        std::abort();
    }
    return std::move(post).value();
}

lora::model::PostPayloadInput received_post_input() {
    lora::model::PostPayloadInput input;
    input.message_id = test_uuid(0x31U);
    input.sender_id = test_uuid(0x71U);
    input.sender_sequence = 1U;
    input.sender_user_id = "Sora";
    input.body = "received only";
    return input;
}

} // namespace

int main() {
    using app::PersistentSession;
    using app::PersistentSessionStatus;
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::model::PostDraft;
    using lora::persistence::StoredLocale;
    lora::test::Runner runner;

    runner.run("first launch and restart preserve identity locale and history", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());

        std::string first_install_id;
        {
            PersistentSession first;
            REQUIRE(first.ready());
            REQUIRE(first.state().identity().has_value());
            first_install_id =
                first.state().identity()->install_id().to_string();
            CHECK_EQ(first.locale(), StoredLocale::English);
            CHECK(!first.skip_title());
            const auto composed = first.state().compose(
                PostDraft{"persisted body", {}, std::nullopt});
            REQUIRE(composed.ok());
            CHECK(first.persist_locale(StoredLocale::Japanese));
            CHECK(first.persist_skip_title(true));
            CHECK_EQ(first.state().timeline().size(), 1U);
            first.shutdown();
        }

        {
            PersistentSession second;
            REQUIRE(second.ready());
            REQUIRE(second.state().identity().has_value());
            CHECK_EQ(second.state().identity()->install_id().to_string(),
                     first_install_id);
            CHECK_EQ(
                second.state().identity()->last_issued_sender_sequence(), 1U);
            CHECK_EQ(second.locale(), StoredLocale::Japanese);
            CHECK(second.skip_title());
            CHECK_EQ(second.state().timeline().size(), 1U);
            const auto* restored = second.state().timeline().newest_at(0);
            REQUIRE(restored != nullptr);
            CHECK_EQ(restored->post.body().value(), "persisted body");
            const auto* delivery =
                std::get_if<LocalDelivery>(&restored->origin);
            REQUIRE(delivery != nullptr);
            CHECK_EQ(delivery->state, LocalDeliveryState::Unknown);

            const auto next = second.state().compose(
                PostDraft{"next body", {}, std::nullopt});
            REQUIRE(next.ok());
            const auto* next_entry =
                second.state().timeline().find(*next.message_id);
            REQUIRE(next_entry != nullptr);
            CHECK_EQ(next_entry->post.sender_sequence(), 2U);
        }
    });

    runner.run("received canonical post and dedupe commit survive restart",
               [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        auto input = received_post_input();
        auto post = lora::model::PostPayload::create(input);
        REQUIRE(post);
        auto encoded = lora::protocol::encode_post(post.value());
        REQUIRE(encoded);

        {
            PersistentSession first;
            REQUIRE(first.ready());
            CHECK_EQ(first.accept_received_post(encoded.value()),
                     app::InboundPostStatus::Accepted);
            REQUIRE(first.state().timeline().size() == 1U);
            const auto order =
                first.state().timeline().last_assigned_order();
            const auto settings_before =
                read_file(temporary.settings());
            sqlite3_int64 post_count = 0;
            sqlite3_int64 seen_count = 0;
            std::string seen_high_water;
            REQUIRE(scalar_integer(
                temporary.database(),
                "SELECT count(*) FROM posts;", post_count));
            REQUIRE(scalar_integer(
                temporary.database(),
                "SELECT count(*) FROM seen_messages;", seen_count));
            REQUIRE(scalar_text(
                temporary.database(),
                "SELECT hex(value) FROM meta"
                " WHERE key='last_seen_order';",
                seen_high_water));
            CHECK_EQ(post_count, 1);
            CHECK_EQ(seen_count, 1);
            CHECK_EQ(seen_high_water, "0000000000000001");

            CHECK_EQ(first.accept_received_post(encoded.value()),
                     app::InboundPostStatus::Duplicate);
            CHECK_EQ(first.state().timeline().size(), 1U);
            CHECK_EQ(first.state().timeline().last_assigned_order(),
                     order);
            CHECK_EQ(read_file(temporary.settings()),
                     settings_before);

            input.body = "conflicting payload";
            auto conflict = lora::model::PostPayload::create(input);
            REQUIRE(conflict);
            auto conflict_encoded =
                lora::protocol::encode_post(conflict.value());
            REQUIRE(conflict_encoded);
            CHECK_EQ(
                first.accept_received_post(conflict_encoded.value()),
                app::InboundPostStatus::Conflict);
            CHECK_EQ(first.state().timeline().size(), 1U);
            CHECK_EQ(first.state().timeline().last_assigned_order(),
                     order);
            CHECK_EQ(read_file(temporary.settings()),
                     settings_before);
            REQUIRE(scalar_integer(
                temporary.database(),
                "SELECT count(*) FROM posts;", post_count));
            REQUIRE(scalar_integer(
                temporary.database(),
                "SELECT count(*) FROM seen_messages;", seen_count));
            REQUIRE(scalar_text(
                temporary.database(),
                "SELECT hex(value) FROM meta"
                " WHERE key='last_seen_order';",
                seen_high_water));
            CHECK_EQ(post_count, 1);
            CHECK_EQ(seen_count, 1);
            CHECK_EQ(seen_high_water, "0000000000000001");
        }

        {
            PersistentSession restarted;
            REQUIRE(restarted.ready());
            const auto settings_before =
                read_file(temporary.settings());
            CHECK_EQ(restarted.accept_received_post(encoded.value()),
                     app::InboundPostStatus::Duplicate);
            CHECK_EQ(restarted.state().timeline().size(), 1U);
            CHECK_EQ(
                restarted.state().timeline().last_assigned_order(),
                1U);
            CHECK_EQ(read_file(temporary.settings()),
                     settings_before);
            sqlite3_int64 seen_count = 0;
            std::string seen_high_water;
            REQUIRE(scalar_integer(
                temporary.database(),
                "SELECT count(*) FROM seen_messages;", seen_count));
            REQUIRE(scalar_text(
                temporary.database(),
                "SELECT hex(value) FROM meta"
                " WHERE key='last_seen_order';",
                seen_high_water));
            CHECK_EQ(seen_count, 1);
            CHECK_EQ(seen_high_water, "0000000000000001");
        }
    });

    runner.run("durable dedupe retains 2048 records and evicts at 2049",
               [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        std::optional<lora::model::Identity> identity;
        {
            PersistentSession initialized;
            REQUIRE(initialized.ready());
            REQUIRE(initialized.state().identity().has_value());
            identity = *initialized.state().identity();
        }
        REQUIRE(identity.has_value());

        lora::adapters::storage::HistorySnapshot snapshot;
        snapshot.last_assigned_order =
            lora::protocol::kDefaultSeenMessageCapacity;
        snapshot.last_seen_order =
            lora::protocol::kDefaultSeenMessageCapacity;
        lora::protocol::Bytes oldest_encoded;
        for (std::size_t index = 0U;
             index < lora::protocol::kDefaultSeenMessageCapacity;
             ++index) {
            auto post = indexed_received_post(
                static_cast<std::uint32_t>(index));
            auto encoded = lora::protocol::encode_post(post);
            REQUIRE(encoded);
            const auto order =
                static_cast<std::uint64_t>(index) + 1U;
            if (index == 0U) {
                oldest_encoded = encoded.value();
            }
            snapshot.seen_messages.push_back(
                lora::protocol::SeenMessageRecord{
                    post.message_id(), order, encoded.value()});
            if (index >=
                lora::protocol::kDefaultSeenMessageCapacity -
                    lora::core::kMaxTimelineEntries) {
                snapshot.entries.push_back(
                    lora::model::TimelineEntry{
                        std::move(post), order,
                        lora::model::ReceivedOrigin{}});
            }
        }
        {
            auto store =
                lora::adapters::storage::SqliteHistoryStore::
                    open_existing_bound(temporary.database(), *identity);
            REQUIRE(store);
            REQUIRE(store.value().save_snapshot(snapshot, *identity) ==
                    lora::adapters::storage::HistoryError::None);
        }

        lora::protocol::Bytes newest_encoded;
        {
            PersistentSession restored;
            REQUIRE(restored.ready());
            CHECK_EQ(restored.state().timeline().size(),
                     lora::core::kMaxTimelineEntries);
            CHECK_EQ(restored.state().timeline().last_assigned_order(),
                     lora::protocol::kDefaultSeenMessageCapacity);
            CHECK_EQ(restored.accept_received_post(oldest_encoded),
                     app::InboundPostStatus::Duplicate);
            CHECK_EQ(restored.state().timeline().last_assigned_order(),
                     lora::protocol::kDefaultSeenMessageCapacity);

            auto newest = indexed_received_post(
                static_cast<std::uint32_t>(
                    lora::protocol::kDefaultSeenMessageCapacity));
            auto encoded = lora::protocol::encode_post(newest);
            REQUIRE(encoded);
            newest_encoded = encoded.value();
            CHECK_EQ(restored.accept_received_post(newest_encoded),
                     app::InboundPostStatus::Accepted);
            CHECK_EQ(restored.state().timeline().size(),
                     lora::core::kMaxTimelineEntries);
            CHECK_EQ(restored.state().timeline().last_assigned_order(),
                     lora::protocol::kDefaultSeenMessageCapacity + 1U);
        }

        sqlite3_int64 seen_count = 0;
        std::string first_retained_order;
        std::string last_retained_order;
        REQUIRE(scalar_integer(
            temporary.database(),
            "SELECT count(*) FROM seen_messages;", seen_count));
        REQUIRE(scalar_text(
            temporary.database(),
            "SELECT hex(min(seen_order)) FROM seen_messages;",
            first_retained_order));
        REQUIRE(scalar_text(
            temporary.database(),
            "SELECT hex(max(seen_order)) FROM seen_messages;",
            last_retained_order));
        CHECK_EQ(
            seen_count,
            static_cast<sqlite3_int64>(
                lora::protocol::kDefaultSeenMessageCapacity));
        CHECK_EQ(first_retained_order, "0000000000000002");
        CHECK_EQ(last_retained_order, "0000000000000801");

        PersistentSession restarted;
        REQUIRE(restarted.ready());
        const auto order_before =
            restarted.state().timeline().last_assigned_order();
        CHECK_EQ(restarted.accept_received_post(newest_encoded),
                 app::InboundPostStatus::Duplicate);
        CHECK_EQ(restarted.state().timeline().last_assigned_order(),
                 order_before);
    });

    runner.run("corrupt settings are preserved until confirmed deletion", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }
        REQUIRE(std::filesystem::exists(temporary.database()));
        write_file(temporary.settings(), "{broken");
        const auto corrupt_bytes = read_file(temporary.settings());

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK_EQ(read_file(temporary.settings()), corrupt_bytes);
        CHECK(std::filesystem::exists(temporary.database()));
        CHECK(recovery.delete_all_local_data());
        CHECK(!std::filesystem::exists(temporary.settings()));
        CHECK(!std::filesystem::exists(temporary.database()));
        recovery.shutdown();

        PersistentSession recreated;
        CHECK(recreated.ready());
    });

    runner.run("missing settings with existing history never rotates identity", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }
        REQUIRE(std::filesystem::exists(temporary.database()));
        REQUIRE(std::filesystem::remove(temporary.settings()));

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK(std::filesystem::exists(temporary.database()));
        CHECK(!std::filesystem::exists(temporary.settings()));
    });

    runner.run("initialized settings with missing empty history require recovery", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }
        const auto initialized_settings = read_file(temporary.settings());
        CHECK(initialized_settings.find(
                  "\"history\":{\"initialized\":true}") !=
              std::string::npos);
        REQUIRE(std::filesystem::remove(temporary.database()));

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK_EQ(read_file(temporary.settings()), initialized_settings);
        CHECK(!std::filesystem::exists(temporary.database()));
    });

    runner.run("interrupted first launch finalizes its existing bound history", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        std::string install_id;
        {
            PersistentSession created;
            REQUIRE(created.ready());
            REQUIRE(created.state().identity().has_value());
            install_id =
                created.state().identity()->install_id().to_string();
        }

        auto interrupted_settings = read_file(temporary.settings());
        const std::string generation = "\"generation\":\"2\"";
        const auto generation_position =
            interrupted_settings.find(generation);
        REQUIRE(generation_position != std::string::npos);
        interrupted_settings.replace(
            generation_position, generation.size(),
            "\"generation\":\"1\"");
        const std::string initialized = "\"initialized\":true";
        const auto marker_position =
            interrupted_settings.find(initialized);
        REQUIRE(marker_position != std::string::npos);
        interrupted_settings.replace(
            marker_position, initialized.size(), "\"initialized\":false");
        write_file(temporary.settings(), interrupted_settings);

        PersistentSession resumed;
        REQUIRE(resumed.ready());
        REQUIRE(resumed.state().identity().has_value());
        CHECK_EQ(resumed.state().identity()->install_id().to_string(),
                 install_id);
        const auto finalized_settings = read_file(temporary.settings());
        CHECK(finalized_settings.find("\"generation\":\"2\"") !=
              std::string::npos);
        CHECK(finalized_settings.find("\"initialized\":true") !=
              std::string::npos);
    });

    runner.run("schema-only history is never accepted as initialized", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }
        REQUIRE(std::filesystem::remove(temporary.database()));
        {
            auto schema_only =
                lora::adapters::storage::SqliteHistoryStore::open(
                    temporary.database());
            REQUIRE(schema_only);
        }
        const auto database_before = read_file(temporary.database());
        REQUIRE(!database_before.empty());

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK_EQ(read_file(temporary.database()), database_before);
    });

    runner.run("stale managed history probes are recovered and deleted", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }

        const auto probe =
            lora::adapters::storage::history_probe_directory_path(
                temporary.database());
        const auto probe_main =
            probe / temporary.database().filename();
        std::error_code error;
        REQUIRE(std::filesystem::create_directory(probe, error));
        REQUIRE(!error);
        write_file(probe_main, "stale-main");
        write_file(probe_main.string() + "-wal", "stale-wal");
        write_file(probe_main.string() + "-journal", "stale-journal");

        PersistentSession resumed;
        REQUIRE(resumed.ready());
        CHECK(!std::filesystem::exists(probe));

        REQUIRE(std::filesystem::create_directory(probe, error));
        REQUIRE(!error);
        write_file(probe_main, "interrupted-probe");
        write_file(probe_main.string() + "-shm", "interrupted-shm");
        CHECK(resumed.delete_all_local_data());
        CHECK(!std::filesystem::exists(probe));
        CHECK(!std::filesystem::exists(temporary.settings()));
        CHECK(!std::filesystem::exists(temporary.database()));
    });

    runner.run("received-only history loss recovers with sender high water zero", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
            auto received =
                lora::model::PostPayload::create(received_post_input());
            REQUIRE(received.has_value());
            REQUIRE(created.state()
                        .accept_received(std::move(received).value())
                        .ok());
            REQUIRE(created.state().identity().has_value());
            CHECK_EQ(
                created.state().identity()->last_issued_sender_sequence(), 0U);
            CHECK_EQ(created.state().timeline().size(), 1U);
        }
        const auto initialized_settings = read_file(temporary.settings());
        CHECK(initialized_settings.find(
                  "\"sender_sequence_high_watermark\":\"0\"") !=
              std::string::npos);
        CHECK(initialized_settings.find(
                  "\"history\":{\"initialized\":true}") !=
              std::string::npos);
        REQUIRE(std::filesystem::remove(temporary.database()));

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK_EQ(read_file(temporary.settings()), initialized_settings);
        CHECK(!std::filesystem::exists(temporary.database()));
    });

    runner.run("orphan history sidecar requires recovery", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        {
            PersistentSession created;
            REQUIRE(created.ready());
        }
        REQUIRE(std::filesystem::remove(temporary.database()));
        auto interrupted_settings = read_file(temporary.settings());
        const std::string initialized = "\"initialized\":true";
        const auto marker = interrupted_settings.find(initialized);
        REQUIRE(marker != std::string::npos);
        interrupted_settings.replace(
            marker, initialized.size(), "\"initialized\":false");
        write_file(temporary.settings(), interrupted_settings);
        const auto orphan =
            std::filesystem::path(temporary.database().string() + "-wal");
        write_file(orphan, "orphan-wal");

        PersistentSession recovery;
        CHECK_EQ(recovery.status(),
                 PersistentSessionStatus::RecoveryRequired);
        CHECK_EQ(read_file(temporary.settings()), interrupted_settings);
        CHECK_EQ(read_file(orphan), "orphan-wal");
        CHECK(!std::filesystem::exists(temporary.database()));
    });

    runner.run("confirmed deletion preserves unrelated neighboring files", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        PersistentSession session;
        REQUIRE(session.ready());
        const auto config_neighbor =
            temporary.config() / "lora-messenger" / "keep.txt";
        const auto data_neighbor =
            temporary.data() / "lora-messenger" / "keep.bin";
        write_file(config_neighbor, "keep-config");
        write_file(data_neighbor, "keep-data");

        CHECK(session.delete_all_local_data());
        CHECK_EQ(read_file(config_neighbor), "keep-config");
        CHECK_EQ(read_file(data_neighbor), "keep-data");
        CHECK(!std::filesystem::exists(temporary.settings()));
        CHECK(!std::filesystem::exists(temporary.database()));
    });

    runner.run("lifetime lock rejects a concurrent second session", [&] {
        TemporaryDirectory temporary;
        EnvironmentGuard environment(temporary.config(), temporary.data());
        PersistentSession first;
        REQUIRE(first.ready());
        PersistentSession second;
        CHECK_EQ(second.status(), PersistentSessionStatus::StorageBusy);
        CHECK(first.ready());
        first.shutdown();

        PersistentSession third;
        CHECK(third.ready());
    });

    runner.run("distinct config homes sharing data contend on history lock", [&] {
        TemporaryDirectory temporary;
        const auto first_config = temporary.root() / "config-a";
        const auto second_config = temporary.root() / "config-b";
        {
            EnvironmentGuard first_environment(
                first_config, temporary.data());
            PersistentSession first;
            REQUIRE(first.ready());

            {
                EnvironmentGuard second_environment(
                    second_config, temporary.data());
                PersistentSession second;
                CHECK_EQ(second.status(),
                         PersistentSessionStatus::StorageBusy);
            }

            CHECK(first.ready());
        }
    });

    return runner.finish();
}
