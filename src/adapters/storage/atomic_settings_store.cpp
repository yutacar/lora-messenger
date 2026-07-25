/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/atomic_settings_store.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lora::adapters::storage {
namespace {

int close_descriptor(int descriptor) noexcept {
    if (descriptor < 0) {
        return 0;
    }
    return ::close(descriptor);
}

bool sync_descriptor(int descriptor, int& error) noexcept {
    while (::fsync(descriptor) != 0) {
        if (errno == EINTR) {
            continue;
        }
        error = errno;
        return false;
    }
    return true;
}

bool rename_file(const std::filesystem::path& from,
                 const std::filesystem::path& to,
                 int& error) noexcept {
    while (::rename(from.c_str(), to.c_str()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        error = errno;
        return false;
    }
    return true;
}

int common_open_flags() noexcept {
    int flags = 0;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

int directory_open_flags() noexcept {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    return flags;
}

SettingsStoreFailure system_failure(SettingsStoreError error,
                                    int system_error,
                                    bool final_replaced = false) {
    return SettingsStoreFailure{
        error,
        system_error,
        std::nullopt,
        std::nullopt,
        final_replaced,
    };
}

SettingsStoreFailure json_failure(SettingsStoreError error,
                                  SettingsJsonError json_error) {
    return SettingsStoreFailure{
        error,
        0,
        json_error,
        std::nullopt,
        false,
    };
}

SettingsStoreFailure injected_failure(AtomicSettingsWritePoint point,
                                      bool final_replaced) {
    return SettingsStoreFailure{
        SettingsStoreError::InjectedFailure,
        0,
        std::nullopt,
        point,
        final_replaced,
    };
}

template<typename T>
core::Result<T, SettingsStoreFailure> failure(SettingsStoreFailure value) {
    return core::Result<T, SettingsStoreFailure>::failure(std::move(value));
}

} // namespace

AtomicSettingsStore::AtomicSettingsStore(
    std::filesystem::path final_path,
    IAtomicSettingsFailpoint* failpoint)
    : final_path_(std::move(final_path)), failpoint_(failpoint) {}

core::Result<persistence::SettingsRecord, SettingsStoreFailure>
AtomicSettingsStore::load() const {
    if (!valid_path()) {
        return failure<persistence::SettingsRecord>(
            system_failure(SettingsStoreError::InvalidPath, EINVAL));
    }

    const int descriptor =
        ::open(final_path_.c_str(),
               O_RDONLY | O_NONBLOCK | common_open_flags());
    if (descriptor < 0) {
        const int error = errno;
        return failure<persistence::SettingsRecord>(system_failure(
            error == ENOENT ? SettingsStoreError::NotFound
                            : SettingsStoreError::OpenSettingsFailed,
            error));
    }

    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const int error = errno;
        static_cast<void>(close_descriptor(descriptor));
        return failure<persistence::SettingsRecord>(
            system_failure(SettingsStoreError::ReadSettingsFailed, error));
    }
    if (!S_ISREG(status.st_mode)) {
        static_cast<void>(close_descriptor(descriptor));
        return failure<persistence::SettingsRecord>(
            system_failure(SettingsStoreError::OpenSettingsFailed, EINVAL));
    }
    if (status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) >
            static_cast<std::uintmax_t>(kMaxSettingsJsonBytes)) {
        static_cast<void>(close_descriptor(descriptor));
        return failure<persistence::SettingsRecord>(
            json_failure(SettingsStoreError::DecodeFailed,
                         SettingsJsonError::TooLarge));
    }

    std::string contents;
    contents.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 4096U> buffer{};
    while (true) {
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            if (contents.size() > kMaxSettingsJsonBytes - size) {
                static_cast<void>(close_descriptor(descriptor));
                return failure<persistence::SettingsRecord>(
                    json_failure(SettingsStoreError::DecodeFailed,
                                 SettingsJsonError::TooLarge));
            }
            contents.append(buffer.data(), size);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        const int error = errno;
        static_cast<void>(close_descriptor(descriptor));
        return failure<persistence::SettingsRecord>(
            system_failure(SettingsStoreError::ReadSettingsFailed, error));
    }

    if (close_descriptor(descriptor) != 0) {
        return failure<persistence::SettingsRecord>(
            system_failure(SettingsStoreError::CloseSettingsFailed, errno));
    }

    auto parsed = parse_settings_json(contents);
    if (!parsed) {
        return failure<persistence::SettingsRecord>(
            json_failure(SettingsStoreError::DecodeFailed, parsed.error()));
    }
    return core::Result<persistence::SettingsRecord,
                        SettingsStoreFailure>::success(
        std::move(parsed).value());
}

core::Result<bool, SettingsStoreFailure>
AtomicSettingsStore::save(
    const persistence::SettingsRecord& settings) const {
    if (!valid_path()) {
        return failure<bool>(
            system_failure(SettingsStoreError::InvalidPath, EINVAL));
    }

    auto encoded = serialize_settings_json(settings);
    if (!encoded) {
        return failure<bool>(
            json_failure(SettingsStoreError::EncodeFailed, encoded.error()));
    }
    const std::string& contents = encoded.value();

    const auto parent = final_path_.parent_path();
    std::error_code directory_error;
    std::filesystem::create_directories(parent, directory_error);
    if (directory_error) {
        return failure<bool>(system_failure(
            SettingsStoreError::CreateDirectoryFailed,
            directory_error.value()));
    }

    int directory_descriptor =
        ::open(parent.c_str(), directory_open_flags());
    if (directory_descriptor < 0) {
        return failure<bool>(system_failure(
            SettingsStoreError::OpenDirectoryFailed, errno));
    }

    int temporary_descriptor = -1;
    const auto temporary = temporary_path();
    bool final_replaced = false;

    const auto cleanup_before_replace = [&]() noexcept {
        if (temporary_descriptor >= 0) {
            static_cast<void>(close_descriptor(temporary_descriptor));
            temporary_descriptor = -1;
        }
        static_cast<void>(::unlink(temporary.c_str()));
        static_cast<void>(close_descriptor(directory_descriptor));
        directory_descriptor = -1;
    };
    const auto close_directory = [&]() noexcept {
        const int result = close_descriptor(directory_descriptor);
        directory_descriptor = -1;
        return result;
    };
    const auto is_injected = [&](AtomicSettingsWritePoint point) noexcept {
        return failpoint_ != nullptr && failpoint_->should_fail(point);
    };
    const auto injected = [&](AtomicSettingsWritePoint point) {
        const auto error = injected_failure(point, final_replaced);
        if (!final_replaced) {
            cleanup_before_replace();
        } else {
            static_cast<void>(close_directory());
        }
        return failure<bool>(error);
    };

    if (is_injected(AtomicSettingsWritePoint::ParentDirectoryOpened)) {
        return injected(AtomicSettingsWritePoint::ParentDirectoryOpened);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeTemporaryOpen)) {
        return injected(AtomicSettingsWritePoint::BeforeTemporaryOpen);
    }

    if (::unlink(temporary.c_str()) != 0 && errno != ENOENT) {
        const int error = errno;
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::RemoveStaleTemporaryFailed, error));
    }

    int temporary_flags = O_WRONLY | O_CREAT | O_EXCL;
    temporary_flags |= common_open_flags();
    temporary_descriptor =
        ::open(temporary.c_str(), temporary_flags, S_IRUSR | S_IWUSR);
    if (temporary_descriptor < 0) {
        const int error = errno;
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::OpenTemporaryFailed, error));
    }
    if (::fchmod(temporary_descriptor, S_IRUSR | S_IWUSR) != 0) {
        const int error = errno;
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::SetTemporaryPermissionsFailed, error));
    }
    if (is_injected(AtomicSettingsWritePoint::TemporaryOpened)) {
        return injected(AtomicSettingsWritePoint::TemporaryOpened);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeWrite)) {
        return injected(AtomicSettingsWritePoint::BeforeWrite);
    }

    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto count =
            ::write(temporary_descriptor, contents.data() + offset,
                    contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        const int error = count == 0 ? EIO : errno;
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::WriteTemporaryFailed, error));
    }
    if (is_injected(AtomicSettingsWritePoint::TemporaryWritten)) {
        return injected(AtomicSettingsWritePoint::TemporaryWritten);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeFileSync)) {
        return injected(AtomicSettingsWritePoint::BeforeFileSync);
    }

    int sync_error = 0;
    if (!sync_descriptor(temporary_descriptor, sync_error)) {
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::SyncTemporaryFailed, sync_error));
    }
    if (is_injected(AtomicSettingsWritePoint::TemporarySynced)) {
        return injected(AtomicSettingsWritePoint::TemporarySynced);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeTemporaryClose)) {
        return injected(AtomicSettingsWritePoint::BeforeTemporaryClose);
    }

    const int descriptor_to_close = temporary_descriptor;
    temporary_descriptor = -1;
    if (close_descriptor(descriptor_to_close) != 0) {
        const int error = errno;
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::CloseTemporaryFailed, error));
    }
    if (is_injected(AtomicSettingsWritePoint::TemporaryClosed)) {
        return injected(AtomicSettingsWritePoint::TemporaryClosed);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeRename)) {
        return injected(AtomicSettingsWritePoint::BeforeRename);
    }

    int rename_error = 0;
    if (!rename_file(temporary, final_path_, rename_error)) {
        cleanup_before_replace();
        return failure<bool>(system_failure(
            SettingsStoreError::RenameFailed, rename_error));
    }
    final_replaced = true;
    if (is_injected(AtomicSettingsWritePoint::FinalReplaced)) {
        return injected(AtomicSettingsWritePoint::FinalReplaced);
    }
    if (is_injected(AtomicSettingsWritePoint::BeforeDirectorySync)) {
        return injected(AtomicSettingsWritePoint::BeforeDirectorySync);
    }

    int directory_sync_error = 0;
    if (!sync_descriptor(directory_descriptor, directory_sync_error)) {
        static_cast<void>(close_directory());
        return failure<bool>(system_failure(
            SettingsStoreError::SyncDirectoryFailed,
            directory_sync_error, true));
    }
    if (is_injected(AtomicSettingsWritePoint::DirectorySynced)) {
        return injected(AtomicSettingsWritePoint::DirectorySynced);
    }
    if (close_directory() != 0) {
        return failure<bool>(system_failure(
            SettingsStoreError::CloseDirectoryFailed, errno, true));
    }

    return core::Result<bool, SettingsStoreFailure>::success(true);
}

const std::filesystem::path&
AtomicSettingsStore::final_path() const noexcept {
    return final_path_;
}

std::filesystem::path AtomicSettingsStore::temporary_path() const {
    return std::filesystem::path(final_path_.string() + ".tmp");
}

bool AtomicSettingsStore::valid_path() const noexcept {
    return final_path_.is_absolute() && !final_path_.filename().empty() &&
           final_path_.filename() != "." &&
           final_path_.filename() != "..";
}

} // namespace lora::adapters::storage
