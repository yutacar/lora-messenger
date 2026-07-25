/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/atomic_settings_store.h"
#include "platform/xdg_paths.h"

#include "../unit/test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using lora::adapters::storage::AtomicSettingsStore;
using lora::adapters::storage::AtomicSettingsWritePoint;
using lora::adapters::storage::IAtomicSettingsFailpoint;
using lora::persistence::SettingsRecord;
using lora::persistence::StoredLocale;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::array<char, 40U> pattern{};
        const std::string value = "/tmp/lora-settings-test-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class SingleFailpoint final : public IAtomicSettingsFailpoint {
public:
    explicit SingleFailpoint(AtomicSettingsWritePoint target) noexcept
        : target_(target) {}

    bool should_fail(AtomicSettingsWritePoint point) noexcept override {
        visited_.push_back(point);
        return point == target_;
    }

    bool visited_target() const noexcept {
        for (const auto point : visited_) {
            if (point == target_) {
                return true;
            }
        }
        return false;
    }

private:
    AtomicSettingsWritePoint target_;
    std::vector<AtomicSettingsWritePoint> visited_;
};

SettingsRecord make_record(std::uint64_t generation,
                           std::uint64_t sequence = 0U,
                           StoredLocale locale = StoredLocale::English,
                           bool history_initialized = true) {
    auto user_id = lora::core::UserId::create("Mika");
    return SettingsRecord{
        generation,
        lora::test::make_install_id(0x20U),
        std::move(user_id).value(),
        sequence,
        locale,
        history_initialized,
    };
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    using lora::adapters::storage::SettingsJsonError;
    using lora::adapters::storage::SettingsStoreError;
    using lora::adapters::storage::kMaxSettingsJsonBytes;
    using lora::platform::XdgEnvironment;
    using lora::platform::XdgPathError;
    using lora::platform::resolve_xdg_paths;
    lora::test::Runner runner;

    runner.run("save creates parents replaces temp and loads exact record", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto final =
            temporary.path() / "new" / "config" / "settings.json";
        AtomicSettingsStore store(final);
        const auto settings =
            make_record(1U, 77U, StoredLocale::Japanese);

        const auto saved = store.save(settings);
        REQUIRE(saved.has_value());
        CHECK(saved.value());
        CHECK(std::filesystem::exists(final));
        CHECK(!std::filesystem::exists(store.temporary_path()));

        const auto loaded = store.load();
        REQUIRE(loaded.has_value());
        CHECK_EQ(loaded.value(), settings);

        struct stat status {};
        REQUIRE(::stat(final.c_str(), &status) == 0);
        CHECK_EQ(status.st_mode & 0777, 0600);
    });

    runner.run("existing temp is replaced and forced to mode 0600", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto final = temporary.path() / "settings.json";
        AtomicSettingsStore store(final);
        write_file(store.temporary_path(), "stale secret");
        REQUIRE(::chmod(store.temporary_path().c_str(), 0666) == 0);

        const auto saved = store.save(make_record(1U));
        REQUIRE(saved.has_value());
        CHECK(!std::filesystem::exists(store.temporary_path()));

        struct stat status {};
        REQUIRE(::stat(final.c_str(), &status) == 0);
        CHECK_EQ(status.st_mode & 0777, 0600);
    });

    runner.run("stale temp hard link cannot truncate the current final", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto final = temporary.path() / "settings.json";
        AtomicSettingsStore baseline(final);
        REQUIRE(baseline.save(make_record(1U)).has_value());
        const auto original_bytes = read_file(final);

        std::error_code link_error;
        std::filesystem::create_hard_link(
            final, baseline.temporary_path(), link_error);
        REQUIRE(!link_error);

        SingleFailpoint failpoint(
            AtomicSettingsWritePoint::TemporaryOpened);
        AtomicSettingsStore interrupted(final, &failpoint);
        const auto result = interrupted.save(make_record(2U));
        CHECK(!result.has_value());
        CHECK(!result.error().final_replaced);
        CHECK_EQ(read_file(final), original_bytes);
        CHECK(!std::filesystem::exists(interrupted.temporary_path()));
    });

    runner.run("successful replacement stores the caller generation", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        AtomicSettingsStore store(temporary.path() / "settings.json");
        REQUIRE(store.save(make_record(1U)).has_value());
        REQUIRE(store.save(make_record(
                    2U, 1U, StoredLocale::SimplifiedChinese,
                    false)).has_value());

        const auto loaded = store.load();
        REQUIRE(loaded.has_value());
        CHECK_EQ(loaded.value().generation, 2U);
        CHECK_EQ(loaded.value().sender_sequence_high_watermark, 1U);
        CHECK_EQ(loaded.value().locale,
                 StoredLocale::SimplifiedChinese);
        CHECK(!loaded.value().history_initialized);
    });

    runner.run("every injected pre-rename interruption preserves old final bytes", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto final = temporary.path() / "settings.json";
        AtomicSettingsStore baseline_store(final);
        REQUIRE(baseline_store.save(make_record(1U)).has_value());
        const auto original_bytes = read_file(final);

        const std::array pre_rename_points{
            AtomicSettingsWritePoint::ParentDirectoryOpened,
            AtomicSettingsWritePoint::BeforeTemporaryOpen,
            AtomicSettingsWritePoint::TemporaryOpened,
            AtomicSettingsWritePoint::BeforeWrite,
            AtomicSettingsWritePoint::TemporaryWritten,
            AtomicSettingsWritePoint::BeforeFileSync,
            AtomicSettingsWritePoint::TemporarySynced,
            AtomicSettingsWritePoint::BeforeTemporaryClose,
            AtomicSettingsWritePoint::TemporaryClosed,
            AtomicSettingsWritePoint::BeforeRename,
        };
        for (const auto point : pre_rename_points) {
            SingleFailpoint failpoint(point);
            AtomicSettingsStore interrupted(final, &failpoint);
            const auto result = interrupted.save(make_record(2U, 1U));
            CHECK(!result.has_value());
            CHECK_EQ(result.error().error,
                     SettingsStoreError::InjectedFailure);
            CHECK(!result.error().final_replaced);
            REQUIRE(result.error().write_point.has_value());
            CHECK_EQ(*result.error().write_point, point);
            CHECK(failpoint.visited_target());
            CHECK_EQ(read_file(final), original_bytes);
            CHECK(!std::filesystem::exists(interrupted.temporary_path()));

            const auto loaded = baseline_store.load();
            REQUIRE(loaded.has_value());
            CHECK_EQ(loaded.value().generation, 1U);
        }
    });

    runner.run("post-rename failures report replacement as committed", [&] {
        const std::array post_rename_points{
            AtomicSettingsWritePoint::FinalReplaced,
            AtomicSettingsWritePoint::BeforeDirectorySync,
            AtomicSettingsWritePoint::DirectorySynced,
        };
        for (const auto point : post_rename_points) {
            TemporaryDirectory temporary;
            REQUIRE(!temporary.path().empty());
            const auto final = temporary.path() / "settings.json";
            AtomicSettingsStore baseline(final);
            REQUIRE(baseline.save(make_record(1U)).has_value());

            SingleFailpoint failpoint(point);
            AtomicSettingsStore interrupted(final, &failpoint);
            const auto result = interrupted.save(make_record(2U, 1U));
            CHECK(!result.has_value());
            CHECK_EQ(result.error().error,
                     SettingsStoreError::InjectedFailure);
            CHECK(result.error().final_replaced);

            const auto loaded = baseline.load();
            REQUIRE(loaded.has_value());
            CHECK_EQ(loaded.value().generation, 2U);
            CHECK(!std::filesystem::exists(interrupted.temporary_path()));
        }
    });

    runner.run("missing corrupt and oversized files have distinct load errors", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto final = temporary.path() / "settings.json";
        AtomicSettingsStore store(final);

        const auto missing = store.load();
        CHECK(!missing.has_value());
        CHECK_EQ(missing.error().error, SettingsStoreError::NotFound);

        write_file(final, "{not-json");
        const auto corrupt = store.load();
        CHECK(!corrupt.has_value());
        CHECK_EQ(corrupt.error().error, SettingsStoreError::DecodeFailed);
        REQUIRE(corrupt.error().json_error.has_value());
        CHECK_EQ(*corrupt.error().json_error,
                 SettingsJsonError::MalformedJson);

        write_file(final, std::string(kMaxSettingsJsonBytes + 1U, ' '));
        const auto oversized = store.load();
        CHECK(!oversized.has_value());
        CHECK_EQ(oversized.error().error, SettingsStoreError::DecodeFailed);
        REQUIRE(oversized.error().json_error.has_value());
        CHECK_EQ(*oversized.error().json_error,
                 SettingsJsonError::TooLarge);
    });

    runner.run("FIFO settings path is rejected without blocking", [&] {
        TemporaryDirectory temporary;
        REQUIRE(!temporary.path().empty());
        const auto fifo = temporary.path() / "settings.json";
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);

        AtomicSettingsStore store(fifo);
        const auto loaded = store.load();
        CHECK(!loaded.has_value());
        CHECK_EQ(loaded.error().error,
                 SettingsStoreError::OpenSettingsFailed);
    });

    runner.run("relative final paths are rejected without filesystem mutation", [&] {
        AtomicSettingsStore store("relative/settings.json");
        const auto saved = store.save(make_record(1U));
        CHECK(!saved.has_value());
        CHECK_EQ(saved.error().error, SettingsStoreError::InvalidPath);

        const auto loaded = store.load();
        CHECK(!loaded.has_value());
        CHECK_EQ(loaded.error().error, SettingsStoreError::InvalidPath);
    });

    runner.run("XDG resolution requires an absolute HOME", [&] {
        const auto missing = resolve_xdg_paths(XdgEnvironment{
            std::nullopt,
            std::string("/tmp/config"),
            std::string("/tmp/data"),
        });
        CHECK(!missing.has_value());
        CHECK_EQ(missing.error(), XdgPathError::HomeMissing);

        const auto relative = resolve_xdg_paths(XdgEnvironment{
            std::string("relative-home"),
            std::nullopt,
            std::nullopt,
        });
        CHECK(!relative.has_value());
        CHECK_EQ(relative.error(), XdgPathError::HomeNotAbsolute);
    });

    runner.run("relative or empty XDG values fall back under HOME", [&] {
        const auto paths = resolve_xdg_paths(XdgEnvironment{
            std::string("/home/mika"),
            std::string("relative-config"),
            std::string(),
        });
        REQUIRE(paths.has_value());
        CHECK_EQ(paths.value().config_directory,
                 std::filesystem::path(
                     "/home/mika/.config/lora-messenger"));
        CHECK_EQ(paths.value().data_directory,
                 std::filesystem::path(
                     "/home/mika/.local/share/lora-messenger"));
        CHECK_EQ(paths.value().settings_file,
                 std::filesystem::path(
                     "/home/mika/.config/lora-messenger/settings.json"));
        CHECK_EQ(paths.value().history_database,
                 std::filesystem::path(
                     "/home/mika/.local/share/lora-messenger/"
                     "history.sqlite3"));
    });

    runner.run("absolute XDG values override HOME and normalize lexically", [&] {
        const auto paths = resolve_xdg_paths(XdgEnvironment{
            std::string("/home/mika"),
            std::string("/tmp/config/../configuration"),
            std::string("/tmp/data"),
        });
        REQUIRE(paths.has_value());
        CHECK_EQ(paths.value().config_directory,
                 std::filesystem::path(
                     "/tmp/configuration/lora-messenger"));
        CHECK_EQ(paths.value().data_directory,
                 std::filesystem::path("/tmp/data/lora-messenger"));
    });

    return runner.finish();
}
