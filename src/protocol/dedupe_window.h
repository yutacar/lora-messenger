/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "core/uuid.h"
#include "model/post.h"
#include "protocol/post_codec.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lora::protocol {

inline constexpr std::size_t kDefaultSeenMessageCapacity = 2'048;

struct SeenMessageRecord {
    core::MessageId message_id;
    std::uint64_t seen_order = 0;
    Bytes encoded_post;
};

enum class DedupeClassification {
    New,
    Duplicate,
    Conflict,
    Invalid,
};

enum class DedupeError {
    None,
    InvalidCapacity,
    TooManyRecords,
    InvalidRecord,
    DuplicateMessageId,
    InvalidOrder,
    OrderExhausted,
    ProtectedCapacity,
};

class MessageDedupeWindow {
public:
    explicit MessageDedupeWindow(
        std::size_t capacity = kDefaultSeenMessageCapacity);

    static core::Result<MessageDedupeWindow, DedupeError> restore(
        std::size_t capacity, std::uint64_t last_seen_order,
        std::vector<SeenMessageRecord> records);

    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;
    bool empty() const noexcept;
    std::uint64_t last_seen_order() const noexcept;
    const std::vector<SeenMessageRecord>& records() const noexcept;

    DedupeClassification classify(
        const core::MessageId& message_id,
        const Bytes& canonical_post) const noexcept;

    DedupeError remember(
        const model::PostPayload& post, const Bytes& canonical_post,
        const std::vector<core::MessageId>& protected_message_ids = {});

private:
    MessageDedupeWindow(std::size_t capacity,
                        std::uint64_t last_seen_order,
                        std::vector<SeenMessageRecord> records);

    std::size_t capacity_;
    std::uint64_t last_seen_order_{0};
    std::vector<SeenMessageRecord> records_;
};

} // namespace lora::protocol
