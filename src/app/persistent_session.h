/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "application/messenger_state.h"
#include "persistence/settings_record.h"
#include "ports/random.h"
#include "protocol/post_codec.h"

#include <memory>

namespace app {

enum class PersistentSessionStatus {
    Ready,
    RecoveryRequired,
    StorageBusy,
    EnvironmentUnavailable,
    StorageUnavailable,
};

enum class InboundPostStatus {
    Accepted,
    Duplicate,
    Conflict,
    Invalid,
    TimelineFull,
    OrderExhausted,
    NotReady,
    StorageUnavailable,
};

// Owns the one-process local settings/history session. All successful mutating
// commands are already durable when they return; shutdown is therefore a bounded
// close operation rather than a last-minute data flush.
class PersistentSession {
public:
    PersistentSession();
    ~PersistentSession();

    PersistentSession(const PersistentSession&) = delete;
    PersistentSession& operator=(const PersistentSession&) = delete;

    PersistentSessionStatus status() const noexcept;
    bool ready() const noexcept;
    bool recovery_required() const noexcept;

    lora::application::MessengerState& state() noexcept;
    const lora::application::MessengerState& state() const noexcept;
    lora::ports::IRandomBytes& random_source() noexcept;

    lora::persistence::StoredLocale locale() const noexcept;
    bool persist_locale(lora::persistence::StoredLocale locale) noexcept;
    bool skip_title() const noexcept;
    bool persist_skip_title(bool skip_title) noexcept;

    // Accepts one canonical protocol post. Identical retained or evicted
    // duplicates and conflicting payloads never mutate the timeline or disk.
    // Accepted means the received timeline row and full-payload dedupe record
    // were committed in the same SQLite transaction.
    InboundPostStatus accept_received_post(
        const lora::protocol::Bytes& canonical_post) noexcept;

    // Removes exact app-owned leaves only. The process lock remains open until
    // shutdown so another instance cannot race a partially completed deletion.
    bool delete_all_local_data() noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace app
