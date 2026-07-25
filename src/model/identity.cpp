/*
 * SPDX-License-Identifier: MIT
 */

#include "model/identity.h"

#include <limits>
#include <utility>

namespace lora::model {

Identity::Identity(core::InstallId install_id, core::UserId user_id,
                   std::uint64_t last_issued_sender_sequence)
    : install_id_(std::move(install_id)),
      user_id_(std::move(user_id)),
      last_issued_sender_sequence_(last_issued_sender_sequence) {}

Identity Identity::restore(core::InstallId install_id, core::UserId user_id,
                           std::uint64_t last_issued_sender_sequence) {
    return Identity(std::move(install_id), std::move(user_id),
                    last_issued_sender_sequence);
}

const core::InstallId& Identity::install_id() const noexcept {
    return install_id_;
}

const core::UserId& Identity::user_id() const noexcept {
    return user_id_;
}

std::uint64_t Identity::last_issued_sender_sequence() const noexcept {
    return last_issued_sender_sequence_;
}

std::optional<std::uint64_t> Identity::next_sender_sequence() const noexcept {
    if (last_issued_sender_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return last_issued_sender_sequence_ + 1;
}

bool Identity::commit_sender_sequence(std::uint64_t sequence) noexcept {
    const auto expected = next_sender_sequence();
    if (!expected || sequence != *expected) {
        return false;
    }
    last_issued_sender_sequence_ = sequence;
    return true;
}

void Identity::rename(core::UserId user_id) {
    user_id_ = std::move(user_id);
}

} // namespace lora::model
