/*
 * SPDX-License-Identifier: MIT
 */

#include "persistent_session.h"

#include "adapters/storage/atomic_settings_store.h"
#include "adapters/storage/sqlite_history_store.h"
#include "platform/xdg_paths.h"
#include "ports/clock.h"
#include "ports/random.h"
#include "ports/state_commit.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace app {
namespace {

using lora::adapters::storage::AtomicSettingsStore;
using lora::adapters::storage::discard_history_probe;
using lora::adapters::storage::HistoryError;
using lora::adapters::storage::HistorySnapshot;
using lora::adapters::storage::SettingsStoreError;
using lora::adapters::storage::SqliteHistoryStore;
using lora::persistence::SettingsRecord;
using lora::persistence::StoredLocale;

class SystemRandom final : public lora::ports::IRandomBytes {
public:
    bool fill(std::uint8_t* destination, std::size_t size) noexcept override {
        if (destination == nullptr || size == 0U || size > 256U) {
            return false;
        }
        while (::getentropy(destination, size) != 0) {
            if (errno != EINTR) {
                return false;
            }
        }
        return true;
    }
};

class SystemClock final : public lora::ports::IWallClock {
public:
    std::optional<lora::ports::UnixSeconds>
    now_unix_seconds() noexcept override {
        const auto duration =
            std::chrono::system_clock::now().time_since_epoch();
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        if (seconds < std::numeric_limits<lora::ports::UnixSeconds>::min() ||
            seconds > std::numeric_limits<lora::ports::UnixSeconds>::max()) {
            return std::nullopt;
        }
        return static_cast<lora::ports::UnixSeconds>(seconds);
    }
};

bool create_private_directory(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error || !std::filesystem::is_directory(path, error) || error) {
        return false;
    }
    return ::chmod(path.c_str(), S_IRWXU) == 0;
}

int common_open_flags() noexcept {
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

bool sync_directory(const std::filesystem::path& path) noexcept {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    bool synced = false;
    while (::fsync(descriptor) != 0) {
        if (errno != EINTR) {
            static_cast<void>(::close(descriptor));
            return false;
        }
    }
    synced = true;
    if (::close(descriptor) != 0) {
        return false;
    }
    return synced;
}

bool unlink_leaf(const std::filesystem::path& path) noexcept {
    while (::unlink(path.c_str()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return errno == ENOENT;
    }
    return true;
}

lora::model::Identity identity_from(const SettingsRecord& settings) {
    return lora::model::Identity::restore(
        settings.install_id, settings.user_id,
        settings.sender_sequence_high_watermark);
}

bool has_history_artifact(const lora::platform::XdgPaths& paths) noexcept {
    for (const auto& path : {
             paths.history_database,
             std::filesystem::path(paths.history_database.string() + "-wal"),
             std::filesystem::path(paths.history_database.string() + "-shm"),
             std::filesystem::path(paths.history_database.string() + "-journal")}) {
        struct stat status {};
        if (::lstat(path.c_str(), &status) == 0) {
            return true;
        }
        if (errno != ENOENT) {
            return true;
        }
    }
    return false;
}

} // namespace

struct PersistentSession::Impl final : public lora::ports::IStateCommit {
    SystemRandom random;
    SystemClock clock;
    lora::application::MessengerState messenger;
    lora::protocol::MessageDedupeWindow dedupe;
    PersistentSessionStatus startup_status{
        PersistentSessionStatus::StorageUnavailable};
    std::optional<lora::platform::XdgPaths> paths;
    std::optional<AtomicSettingsStore> settings_store;
    std::optional<SettingsRecord> settings;
    std::optional<SqliteHistoryStore> history;
    int config_lock_descriptor{-1};
    int data_lock_descriptor{-1};
    bool closed{false};

    Impl() : messenger(random, clock, lora::core::kMaxTimelineEntries, 0U, this) {
        bootstrap();
    }

    ~Impl() override {
        shutdown();
    }

    bool persist_identity(
        const lora::model::Identity& identity) noexcept override {
        if (startup_status != PersistentSessionStatus::Ready ||
            closed || !settings || !settings_store ||
            settings->generation == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        SettingsRecord candidate{
            settings->generation + 1U,
            identity.install_id(),
            identity.user_id(),
            identity.last_issued_sender_sequence(),
            settings->locale,
            settings->history_initialized,
            settings->skip_title,
        };
        auto saved = settings_store->save(candidate);
        if (!saved) {
            if (saved.error().final_replaced) {
                startup_status = PersistentSessionStatus::RecoveryRequired;
            }
            return false;
        }
        settings = std::move(candidate);
        return true;
    }

    bool persist_timeline(
        const lora::model::Timeline& timeline) noexcept override {
        if (startup_status != PersistentSessionStatus::Ready ||
            closed || !settings || !history) {
            return false;
        }
        try {
            auto candidate_dedupe = dedupe;
            std::vector<lora::core::MessageId> protected_ids;
            protected_ids.reserve(timeline.entries().size());
            for (const auto& entry : timeline.entries()) {
                protected_ids.push_back(entry.post.message_id());
            }
            for (const auto& entry : timeline.entries()) {
                auto encoded =
                    lora::protocol::encode_post(entry.post);
                if (!encoded) {
                    startup_status =
                        PersistentSessionStatus::RecoveryRequired;
                    return false;
                }
                const auto classification =
                    candidate_dedupe.classify(
                        entry.post.message_id(), encoded.value());
                if (classification ==
                        lora::protocol::DedupeClassification::Duplicate &&
                    messenger.timeline().find(
                        entry.post.message_id()) == nullptr) {
                    return false;
                }
                if (classification ==
                        lora::protocol::DedupeClassification::Conflict ||
                    classification ==
                        lora::protocol::DedupeClassification::Invalid ||
                    (classification ==
                         lora::protocol::DedupeClassification::New &&
                     candidate_dedupe.remember(
                         entry.post, encoded.value(), protected_ids) !=
                         lora::protocol::DedupeError::None)) {
                    startup_status =
                        PersistentSessionStatus::RecoveryRequired;
                    return false;
                }
            }

            HistorySnapshot snapshot;
            snapshot.entries = timeline.entries();
            snapshot.last_assigned_order =
                timeline.last_assigned_order();
            snapshot.seen_messages = candidate_dedupe.records();
            snapshot.last_seen_order =
                candidate_dedupe.last_seen_order();
            const auto identity = identity_from(*settings);
            const auto result =
                history->save_snapshot(snapshot, identity);
            if (result != HistoryError::None) {
                startup_status =
                    PersistentSessionStatus::RecoveryRequired;
                return false;
            }
            dedupe = std::move(candidate_dedupe);
            return true;
        } catch (...) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }
    }

    InboundPostStatus accept_received_post(
        const lora::protocol::Bytes& canonical_post) noexcept {
        if (startup_status != PersistentSessionStatus::Ready ||
            closed || !settings || !history) {
            return InboundPostStatus::NotReady;
        }
        try {
            auto decoded =
                lora::protocol::decode_post(canonical_post);
            if (!decoded) {
                return InboundPostStatus::Invalid;
            }
            auto encoded =
                lora::protocol::encode_post(decoded.value());
            if (!encoded || encoded.value() != canonical_post) {
                return InboundPostStatus::Invalid;
            }
            switch (dedupe.classify(
                decoded.value().message_id(), canonical_post)) {
                case lora::protocol::DedupeClassification::Duplicate:
                    return InboundPostStatus::Duplicate;
                case lora::protocol::DedupeClassification::Conflict:
                    return InboundPostStatus::Conflict;
                case lora::protocol::DedupeClassification::Invalid:
                    return InboundPostStatus::Invalid;
                case lora::protocol::DedupeClassification::New:
                    break;
            }

            const auto accepted = messenger.accept_received(
                std::move(decoded).value());
            switch (accepted.error) {
                case lora::application::CommandError::None:
                    return InboundPostStatus::Accepted;
                case lora::application::CommandError::DuplicatePost:
                    return InboundPostStatus::Duplicate;
                case lora::application::CommandError::ConflictingPost:
                    return InboundPostStatus::Conflict;
                case lora::application::CommandError::TimelineFull:
                case lora::application::CommandError::QueueFull:
                    return InboundPostStatus::TimelineFull;
                case lora::application::CommandError::OrderExhausted:
                    return InboundPostStatus::OrderExhausted;
                case lora::application::CommandError::PersistenceUnavailable:
                    return InboundPostStatus::StorageUnavailable;
                default:
                    return InboundPostStatus::Invalid;
            }
        } catch (...) {
            return InboundPostStatus::StorageUnavailable;
        }
    }

    bool set_locale(StoredLocale locale) noexcept {
        if (startup_status != PersistentSessionStatus::Ready ||
            closed || !settings || !settings_store ||
            settings->generation == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (settings->locale == locale) {
            return true;
        }
        auto candidate = *settings;
        ++candidate.generation;
        candidate.locale = locale;
        auto saved = settings_store->save(candidate);
        if (!saved) {
            if (saved.error().final_replaced) {
                startup_status = PersistentSessionStatus::RecoveryRequired;
            }
            return false;
        }
        settings = std::move(candidate);
        return true;
    }

    bool set_skip_title(bool skip_title) noexcept {
        if (startup_status != PersistentSessionStatus::Ready ||
            closed || !settings || !settings_store ||
            settings->generation == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (settings->skip_title == skip_title) {
            return true;
        }
        auto candidate = *settings;
        ++candidate.generation;
        candidate.skip_title = skip_title;
        auto saved = settings_store->save(candidate);
        if (!saved) {
            if (saved.error().final_replaced) {
                startup_status = PersistentSessionStatus::RecoveryRequired;
            }
            return false;
        }
        settings = std::move(candidate);
        return true;
    }

    bool delete_all() noexcept {
        if (!paths || config_lock_descriptor < 0 ||
            data_lock_descriptor < 0 || closed) {
            return false;
        }
        if (discard_history_probe(paths->history_database) !=
            HistoryError::None) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }

        bool settings_removed = true;
        if (settings_store) {
            settings_removed =
                unlink_leaf(settings_store->temporary_path()) &&
                settings_removed;
        }
        settings_removed =
            unlink_leaf(paths->settings_file) && settings_removed;
        settings_removed =
            sync_directory(paths->config_directory) && settings_removed;
        if (!settings_removed) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }
        settings.reset();

        history.reset();
        bool history_removed = true;
        for (const auto& path : {
                 std::filesystem::path(paths->history_database.string() + "-wal"),
                 std::filesystem::path(paths->history_database.string() + "-shm"),
                 std::filesystem::path(paths->history_database.string() + "-journal"),
                 paths->history_database}) {
            history_removed = unlink_leaf(path) && history_removed;
        }
        history_removed =
            sync_directory(paths->data_directory) && history_removed;
        if (!history_removed) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }
        startup_status = PersistentSessionStatus::RecoveryRequired;
        return true;
    }

    void shutdown() noexcept {
        if (closed) {
            return;
        }
        history.reset();
        release_lock(data_lock_descriptor);
        release_lock(config_lock_descriptor);
        closed = true;
    }

private:
    void bootstrap() {
        auto resolved = lora::platform::resolve_xdg_paths_from_environment();
        if (!resolved) {
            startup_status =
                PersistentSessionStatus::EnvironmentUnavailable;
            return;
        }
        paths = std::move(resolved).value();
        if (!create_private_directory(paths->config_directory) ||
            !create_private_directory(paths->data_directory)) {
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return;
        }
        if (!acquire_lock()) {
            return;
        }
        if (discard_history_probe(paths->history_database) !=
            HistoryError::None) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return;
        }

        settings_store.emplace(paths->settings_file);
        auto loaded = settings_store->load();
        if (!loaded) {
            if (loaded.error().error != SettingsStoreError::NotFound) {
                startup_status =
                    PersistentSessionStatus::RecoveryRequired;
                return;
            }
            bootstrap_missing_settings();
            return;
        }
        settings = std::move(loaded).value();
        bootstrap_loaded_settings();
    }

    static void release_lock(int& descriptor) noexcept {
        if (descriptor < 0) {
            return;
        }
        static_cast<void>(::flock(descriptor, LOCK_UN));
        static_cast<void>(::close(descriptor));
        descriptor = -1;
    }

    bool acquire_one_lock(const std::filesystem::path& lock_path,
                          int& descriptor) {
        descriptor =
            ::open(lock_path.c_str(), common_open_flags(), S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return false;
        }

        struct stat status {};
        if (::fstat(descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
            status.st_uid != ::geteuid() ||
            ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
            release_lock(descriptor);
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return false;
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            release_lock(descriptor);
            startup_status =
                lock_error == EWOULDBLOCK || lock_error == EAGAIN
                    ? PersistentSessionStatus::StorageBusy
                    : PersistentSessionStatus::StorageUnavailable;
            return false;
        }
        return true;
    }

    bool acquire_lock() {
        if (!acquire_one_lock(paths->config_directory / "settings.lock",
                              config_lock_descriptor)) {
            return false;
        }
        if (!acquire_one_lock(paths->data_directory / "history.lock",
                              data_lock_descriptor)) {
            release_lock(config_lock_descriptor);
            return false;
        }
        return true;
    }

    void bootstrap_missing_settings() {
        if (has_history_artifact(*paths)) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return;
        }

        auto install_id = lora::core::InstallId::generate(random);
        auto user_id = lora::core::UserId::create("Mika");
        if (!install_id || !user_id) {
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return;
        }
        SettingsRecord created{
            1U,
            std::move(install_id).value(),
            std::move(user_id).value(),
            0U,
            StoredLocale::English,
            false,
            false,
        };
        auto saved = settings_store->save(created);
        if (!saved) {
            startup_status = saved.error().final_replaced
                ? PersistentSessionStatus::RecoveryRequired
                : PersistentSessionStatus::StorageUnavailable;
            return;
        }
        settings = std::move(created);
        if (!open_empty_history()) {
            return;
        }
        if (!finalize_history_initialization()) {
            return;
        }
        const auto identity = identity_from(*settings);
        if (!messenger.restore_state(identity, lora::model::Timeline{}).ok()) {
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return;
        }
        startup_status = PersistentSessionStatus::Ready;
    }

    void bootstrap_loaded_settings() {
        struct stat database_status {};
        const int database_result =
            ::lstat(paths->history_database.c_str(), &database_status);
        const bool database_exists = database_result == 0;
        if (database_result != 0 && errno != ENOENT) {
            startup_status = PersistentSessionStatus::StorageUnavailable;
            return;
        }
        if (!database_exists) {
            // A false marker with sequence zero and no history artifacts is
            // the only resumable first-launch state.
            if (settings->history_initialized ||
                settings->sender_sequence_high_watermark != 0U ||
                has_history_artifact(*paths) ||
                !open_empty_history()) {
                startup_status =
                    PersistentSessionStatus::RecoveryRequired;
                return;
            }
        } else {
            const auto identity = identity_from(*settings);
            if (!S_ISREG(database_status.st_mode) ||
                database_status.st_size == 0 ||
                !open_history(identity)) {
                startup_status =
                    PersistentSessionStatus::RecoveryRequired;
                return;
            }
        }

        auto identity = identity_from(*settings);
        auto loaded = history->load_snapshot(identity);
        if (!loaded) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return;
        }
        if (!settings->history_initialized &&
            !finalize_history_initialization()) {
            return;
        }
        auto snapshot = std::move(loaded).value();
        auto restored_dedupe =
            lora::protocol::MessageDedupeWindow::restore(
                lora::protocol::kDefaultSeenMessageCapacity,
                snapshot.last_seen_order, snapshot.seen_messages);
        if (!restored_dedupe) {
            startup_status =
                PersistentSessionStatus::RecoveryRequired;
            return;
        }
        dedupe = std::move(restored_dedupe).value();
        bool changed_queued = false;
        for (auto& entry : snapshot.entries) {
            auto* local =
                std::get_if<lora::model::LocalDelivery>(&entry.origin);
            if (local &&
                local->state ==
                    lora::model::LocalDeliveryState::Queued) {
                local->state =
                    lora::model::LocalDeliveryState::Unknown;
                changed_queued = true;
            }
        }
        if (changed_queued &&
            history->save_snapshot(snapshot, identity) !=
                HistoryError::None) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return;
        }

        auto timeline = lora::model::Timeline::restore(
            lora::core::kMaxTimelineEntries,
            snapshot.last_assigned_order,
            std::move(snapshot.entries));
        if (!timeline ||
            !messenger.restore_state(
                std::move(identity), std::move(timeline).value()).ok()) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return;
        }
        startup_status = PersistentSessionStatus::Ready;
    }

    bool open_history(const lora::model::Identity& identity) {
        auto opened = SqliteHistoryStore::open_existing_bound(
            paths->history_database, identity);
        if (!opened) {
            return false;
        }
        history.emplace(std::move(opened).value());
        return true;
    }

    bool open_empty_history() {
        auto opened = SqliteHistoryStore::open(paths->history_database);
        if (!opened) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }
        history.emplace(std::move(opened).value());
        const auto identity = identity_from(*settings);
        if (history->save_snapshot(HistorySnapshot{}, identity) !=
            HistoryError::None) {
            history.reset();
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }
        return true;
    }

    bool finalize_history_initialization() {
        if (settings->history_initialized) {
            return true;
        }
        if (settings->generation == std::numeric_limits<std::uint64_t>::max()) {
            startup_status = PersistentSessionStatus::RecoveryRequired;
            return false;
        }

        auto candidate = *settings;
        ++candidate.generation;
        candidate.history_initialized = true;
        auto saved = settings_store->save(candidate);
        if (!saved) {
            startup_status = saved.error().final_replaced
                ? PersistentSessionStatus::RecoveryRequired
                : PersistentSessionStatus::StorageUnavailable;
            return false;
        }
        settings = std::move(candidate);
        return true;
    }
};

PersistentSession::PersistentSession()
    : impl_(std::make_unique<Impl>()) {}

PersistentSession::~PersistentSession() {
    shutdown();
}

PersistentSessionStatus PersistentSession::status() const noexcept {
    return impl_->startup_status;
}

bool PersistentSession::ready() const noexcept {
    return status() == PersistentSessionStatus::Ready;
}

bool PersistentSession::recovery_required() const noexcept {
    return status() == PersistentSessionStatus::RecoveryRequired;
}

lora::application::MessengerState&
PersistentSession::state() noexcept {
    return impl_->messenger;
}

const lora::application::MessengerState&
PersistentSession::state() const noexcept {
    return impl_->messenger;
}

lora::ports::IRandomBytes&
PersistentSession::random_source() noexcept {
    return impl_->random;
}

StoredLocale PersistentSession::locale() const noexcept {
    return impl_->settings
        ? impl_->settings->locale
        : StoredLocale::English;
}

bool PersistentSession::persist_locale(StoredLocale locale) noexcept {
    return impl_->set_locale(locale);
}

bool PersistentSession::skip_title() const noexcept {
    return impl_->settings && impl_->settings->skip_title;
}

bool PersistentSession::persist_skip_title(bool skip_title) noexcept {
    return impl_->set_skip_title(skip_title);
}

InboundPostStatus PersistentSession::accept_received_post(
    const lora::protocol::Bytes& canonical_post) noexcept {
    return impl_->accept_received_post(canonical_post);
}

bool PersistentSession::delete_all_local_data() noexcept {
    return impl_->delete_all();
}

void PersistentSession::shutdown() noexcept {
    if (impl_) {
        impl_->shutdown();
    }
}

} // namespace app
