/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/dedupe_window.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace lora::protocol {
namespace {

bool is_canonical_record(const SeenMessageRecord& record) {
    if (record.seen_order == 0) {
        return false;
    }
    auto decoded = decode_post(record.encoded_post.data(),
                               record.encoded_post.size());
    if (!decoded ||
        decoded.value().message_id() != record.message_id) {
        return false;
    }
    auto encoded = encode_post(decoded.value());
    return encoded && encoded.value() == record.encoded_post;
}

bool is_protected(
    const core::MessageId& message_id,
    const std::vector<core::MessageId>& protected_message_ids) noexcept {
    return std::find(protected_message_ids.begin(),
                     protected_message_ids.end(),
                     message_id) != protected_message_ids.end();
}

} // namespace

MessageDedupeWindow::MessageDedupeWindow(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument(
            "dedupe capacity must be nonzero");
    }
}

MessageDedupeWindow::MessageDedupeWindow(
    std::size_t capacity, std::uint64_t last_seen_order,
    std::vector<SeenMessageRecord> records)
    : capacity_(capacity),
      last_seen_order_(last_seen_order),
      records_(std::move(records)) {}

core::Result<MessageDedupeWindow, DedupeError>
MessageDedupeWindow::restore(
    std::size_t capacity, std::uint64_t last_seen_order,
    std::vector<SeenMessageRecord> records) {
    if (capacity == 0) {
        return core::Result<MessageDedupeWindow, DedupeError>::failure(
            DedupeError::InvalidCapacity);
    }
    if (records.size() > capacity) {
        return core::Result<MessageDedupeWindow, DedupeError>::failure(
            DedupeError::TooManyRecords);
    }

    std::set<core::MessageId> message_ids;
    std::set<std::uint64_t> seen_orders;
    std::uint64_t previous_order = 0;
    for (const auto& record : records) {
        if (!is_canonical_record(record)) {
            return core::Result<MessageDedupeWindow, DedupeError>::failure(
                DedupeError::InvalidRecord);
        }
        if (!message_ids.insert(record.message_id).second) {
            return core::Result<MessageDedupeWindow, DedupeError>::failure(
                DedupeError::DuplicateMessageId);
        }
        if (!seen_orders.insert(record.seen_order).second ||
            record.seen_order <= previous_order ||
            record.seen_order > last_seen_order) {
            return core::Result<MessageDedupeWindow, DedupeError>::failure(
                DedupeError::InvalidOrder);
        }
        previous_order = record.seen_order;
    }
    if ((records.empty() && last_seen_order != 0) ||
        (!records.empty() &&
         records.back().seen_order != last_seen_order)) {
        return core::Result<MessageDedupeWindow, DedupeError>::failure(
            DedupeError::InvalidOrder);
    }

    return core::Result<MessageDedupeWindow, DedupeError>::success(
        MessageDedupeWindow(
            capacity, last_seen_order, std::move(records)));
}

std::size_t MessageDedupeWindow::capacity() const noexcept {
    return capacity_;
}

std::size_t MessageDedupeWindow::size() const noexcept {
    return records_.size();
}

bool MessageDedupeWindow::empty() const noexcept {
    return records_.empty();
}

std::uint64_t MessageDedupeWindow::last_seen_order() const noexcept {
    return last_seen_order_;
}

const std::vector<SeenMessageRecord>&
MessageDedupeWindow::records() const noexcept {
    return records_;
}

DedupeClassification MessageDedupeWindow::classify(
    const core::MessageId& message_id,
    const Bytes& canonical_post) const noexcept {
    const auto found = std::find_if(
        records_.begin(), records_.end(),
        [&message_id](const SeenMessageRecord& record) {
            return record.message_id == message_id;
        });
    if (found == records_.end()) {
        return DedupeClassification::New;
    }
    return found->encoded_post == canonical_post
               ? DedupeClassification::Duplicate
               : DedupeClassification::Conflict;
}

DedupeError MessageDedupeWindow::remember(
    const model::PostPayload& post, const Bytes& canonical_post,
    const std::vector<core::MessageId>& protected_message_ids) {
    auto decoded = decode_post(canonical_post.data(),
                               canonical_post.size());
    if (!decoded || decoded.value() != post) {
        return DedupeError::InvalidRecord;
    }
    auto encoded = encode_post(post);
    if (!encoded || encoded.value() != canonical_post) {
        return DedupeError::InvalidRecord;
    }

    switch (classify(post.message_id(), canonical_post)) {
        case DedupeClassification::Duplicate:
            return DedupeError::None;
        case DedupeClassification::Conflict:
            return DedupeError::DuplicateMessageId;
        case DedupeClassification::Invalid:
            return DedupeError::InvalidRecord;
        case DedupeClassification::New:
            break;
    }

    if (last_seen_order_ == std::numeric_limits<std::uint64_t>::max()) {
        return DedupeError::OrderExhausted;
    }
    if (records_.size() >= capacity_) {
        const auto evicted = std::find_if(
            records_.begin(), records_.end(),
            [&protected_message_ids](const SeenMessageRecord& record) {
                return !is_protected(record.message_id,
                                     protected_message_ids);
            });
        if (evicted == records_.end()) {
            return DedupeError::ProtectedCapacity;
        }
        records_.erase(evicted);
    }

    ++last_seen_order_;
    records_.push_back(SeenMessageRecord{
        post.message_id(), last_seen_order_, canonical_post});
    return DedupeError::None;
}

} // namespace lora::protocol
