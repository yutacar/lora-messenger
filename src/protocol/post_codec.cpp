/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/post_codec.h"

#include "core/limits.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace lora::protocol {
namespace {

constexpr std::uint8_t kReplyPresent = 0x01U;
constexpr std::uint8_t kSenderTimePresent = 0x02U;
constexpr std::uint8_t kKnownFlags =
    kReplyPresent | kSenderTimePresent;
constexpr std::size_t kFixedPostBytes = 44U;

void append_uint64(Bytes& output, std::uint64_t value) {
    for (unsigned shift = 56U;; shift -= 8U) {
        output.push_back(
            static_cast<std::uint8_t>((value >> shift) & 0xffU));
        if (shift == 0U) {
            break;
        }
    }
}

std::uint64_t read_uint64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) |
                static_cast<std::uint64_t>(data[index]);
    }
    return value;
}

std::uint64_t encode_signed(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return std::numeric_limits<std::uint64_t>::max() -
           static_cast<std::uint64_t>(-(value + 1));
}

std::int64_t decode_signed(std::uint64_t value) noexcept {
    const auto signed_max =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
    if (value <= signed_max) {
        return static_cast<std::int64_t>(value);
    }
    return -1 -
           static_cast<std::int64_t>(
               std::numeric_limits<std::uint64_t>::max() - value);
}

void append_uuid(Bytes& output, const core::Uuid& uuid) {
    const auto& bytes = uuid.bytes();
    output.insert(output.end(), bytes.begin(), bytes.end());
}

core::Uuid read_uuid(const std::uint8_t* data) noexcept {
    core::Uuid::Bytes bytes{};
    std::copy_n(data, bytes.size(), bytes.begin());
    return core::Uuid::from_bytes(bytes);
}

} // namespace

core::Result<Bytes, PostCodecError>
encode_post(const model::PostPayload& post) {
    const auto& user_id = post.sender_user_id().value();
    const auto& body = post.body().value();
    const auto& mentions = post.mentions();

    const std::size_t encoded_size =
        kFixedPostBytes + user_id.size() + body.size() +
        mentions.size() * core::Uuid::Bytes{}.size() +
        (post.reply_to() ? core::Uuid::Bytes{}.size() : 0U) +
        (post.sender_time() ? sizeof(std::uint64_t) : 0U);
    if (user_id.size() > core::kMaxUserIdBytes ||
        body.size() > core::kMaxPostBodyBytes ||
        mentions.size() > core::kMaxMentions ||
        encoded_size < kMinimumEncodedPostBytes ||
        encoded_size > kMaximumEncodedPostBytes) {
        return core::Result<Bytes, PostCodecError>::failure(
            PostCodecError::SizeOutOfRange);
    }

    Bytes output;
    output.reserve(encoded_size);

    std::uint8_t flags = 0;
    if (post.reply_to()) {
        flags |= kReplyPresent;
    }
    if (post.sender_time()) {
        flags |= kSenderTimePresent;
    }
    output.push_back(flags);
    append_uuid(output, post.message_id().uuid());
    append_uuid(output, post.sender_id().uuid());
    append_uint64(output, post.sender_sequence());
    output.push_back(static_cast<std::uint8_t>(user_id.size()));
    output.push_back(static_cast<std::uint8_t>(body.size()));
    output.push_back(static_cast<std::uint8_t>(mentions.size()));
    output.insert(output.end(), user_id.begin(), user_id.end());
    output.insert(output.end(), body.begin(), body.end());
    for (const auto& mention : mentions) {
        append_uuid(output, mention.uuid());
    }
    if (post.reply_to()) {
        append_uuid(output, post.reply_to()->uuid());
    }
    if (post.sender_time()) {
        append_uint64(output, encode_signed(*post.sender_time()));
    }

    if (output.size() != encoded_size) {
        return core::Result<Bytes, PostCodecError>::failure(
            PostCodecError::LengthMismatch);
    }
    return core::Result<Bytes, PostCodecError>::success(
        std::move(output));
}

core::Result<model::PostPayload, PostCodecError>
decode_post(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::NullData);
    }
    if (size < kMinimumEncodedPostBytes ||
        size > kMaximumEncodedPostBytes) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::SizeOutOfRange);
    }

    const std::uint8_t flags = data[0];
    if ((flags & static_cast<std::uint8_t>(~kKnownFlags)) != 0U) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::ReservedFlags);
    }

    const std::size_t user_id_size = data[41];
    const std::size_t body_size = data[42];
    const std::size_t mention_count = data[43];
    if (user_id_size > core::kMaxUserIdBytes ||
        body_size > core::kMaxPostBodyBytes ||
        mention_count > core::kMaxMentions) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::LengthMismatch);
    }

    const std::size_t expected_size =
        kFixedPostBytes + user_id_size + body_size +
        mention_count * core::Uuid::Bytes{}.size() +
        ((flags & kReplyPresent) != 0U
             ? core::Uuid::Bytes{}.size()
             : 0U) +
        ((flags & kSenderTimePresent) != 0U
             ? sizeof(std::uint64_t)
             : 0U);
    if (expected_size != size) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::LengthMismatch);
    }

    model::PostPayloadInput input;
    input.message_id = read_uuid(data + 1U);
    input.sender_id = read_uuid(data + 17U);
    input.sender_sequence = read_uint64(data + 33U);

    std::size_t offset = kFixedPostBytes;
    input.sender_user_id.assign(
        reinterpret_cast<const char*>(data + offset), user_id_size);
    offset += user_id_size;
    input.body.assign(
        reinterpret_cast<const char*>(data + offset), body_size);
    offset += body_size;

    input.mentions.reserve(mention_count);
    for (std::size_t index = 0; index < mention_count; ++index) {
        input.mentions.push_back(read_uuid(data + offset));
        offset += core::Uuid::Bytes{}.size();
    }
    if ((flags & kReplyPresent) != 0U) {
        input.reply_to = read_uuid(data + offset);
        offset += core::Uuid::Bytes{}.size();
    }
    if ((flags & kSenderTimePresent) != 0U) {
        input.sender_time = decode_signed(read_uint64(data + offset));
        offset += sizeof(std::uint64_t);
    }
    if (offset != size) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::LengthMismatch);
    }

    auto post = model::PostPayload::create(std::move(input));
    if (!post) {
        return core::Result<model::PostPayload, PostCodecError>::failure(
            PostCodecError::InvalidPost);
    }
    return core::Result<model::PostPayload, PostCodecError>::success(
        std::move(post).value());
}

MessageTag message_tag(const core::MessageId& message_id) noexcept {
    MessageTag tag{};
    const auto& bytes = message_id.uuid().bytes();
    std::copy_n(bytes.begin(), tag.size(), tag.begin());
    return tag;
}

} // namespace lora::protocol
