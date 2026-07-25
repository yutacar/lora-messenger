/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/text.h"
#include "core/uuid.h"

#include <cstdint>
#include <optional>

namespace lora::model {

class Identity {
public:
    static Identity restore(core::InstallId install_id, core::UserId user_id,
                            std::uint64_t last_issued_sender_sequence = 0);

    const core::InstallId& install_id() const noexcept;
    const core::UserId& user_id() const noexcept;
    std::uint64_t last_issued_sender_sequence() const noexcept;

    std::optional<std::uint64_t> next_sender_sequence() const noexcept;
    bool commit_sender_sequence(std::uint64_t sequence) noexcept;
    void rename(core::UserId user_id);

private:
    Identity(core::InstallId install_id, core::UserId user_id,
             std::uint64_t last_issued_sender_sequence);

    core::InstallId install_id_;
    core::UserId user_id_;
    std::uint64_t last_issued_sender_sequence_;
};

} // namespace lora::model
