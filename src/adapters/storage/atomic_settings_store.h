/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "adapters/storage/settings_json.h"
#include "core/result.h"
#include "persistence/settings_record.h"

#include <filesystem>
#include <optional>

namespace lora::adapters::storage {

enum class AtomicSettingsWritePoint {
    ParentDirectoryOpened,
    BeforeTemporaryOpen,
    TemporaryOpened,
    BeforeWrite,
    TemporaryWritten,
    BeforeFileSync,
    TemporarySynced,
    BeforeTemporaryClose,
    TemporaryClosed,
    BeforeRename,
    FinalReplaced,
    BeforeDirectorySync,
    DirectorySynced,
};

class IAtomicSettingsFailpoint {
public:
    virtual ~IAtomicSettingsFailpoint() = default;
    virtual bool should_fail(AtomicSettingsWritePoint point) noexcept = 0;
};

enum class SettingsStoreError {
    InvalidPath,
    NotFound,
    CreateDirectoryFailed,
    OpenDirectoryFailed,
    OpenSettingsFailed,
    ReadSettingsFailed,
    CloseSettingsFailed,
    DecodeFailed,
    EncodeFailed,
    RemoveStaleTemporaryFailed,
    OpenTemporaryFailed,
    SetTemporaryPermissionsFailed,
    WriteTemporaryFailed,
    SyncTemporaryFailed,
    CloseTemporaryFailed,
    RenameFailed,
    SyncDirectoryFailed,
    CloseDirectoryFailed,
    InjectedFailure,
};

struct SettingsStoreFailure {
    SettingsStoreError error;
    int system_error{0};
    std::optional<SettingsJsonError> json_error;
    std::optional<AtomicSettingsWritePoint> write_point;
    bool final_replaced{false};
};

class AtomicSettingsStore {
public:
    explicit AtomicSettingsStore(
        std::filesystem::path final_path,
        IAtomicSettingsFailpoint* failpoint = nullptr);

    core::Result<persistence::SettingsRecord, SettingsStoreFailure>
    load() const;

    core::Result<bool, SettingsStoreFailure>
    save(const persistence::SettingsRecord& settings) const;

    const std::filesystem::path& final_path() const noexcept;
    std::filesystem::path temporary_path() const;

private:
    bool valid_path() const noexcept;

    std::filesystem::path final_path_;
    IAtomicSettingsFailpoint* failpoint_;
};

} // namespace lora::adapters::storage
