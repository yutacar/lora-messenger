/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "core/uuid.h"
#include "model/post.h"
#include "protocol/limits.h"

#include <cstddef>
#include <cstdint>

namespace lora::protocol {

enum class PostCodecError {
    None,
    NullData,
    SizeOutOfRange,
    ReservedFlags,
    LengthMismatch,
    InvalidPost,
};

core::Result<Bytes, PostCodecError>
encode_post(const model::PostPayload& post);

core::Result<model::PostPayload, PostCodecError>
decode_post(const std::uint8_t* data, std::size_t size);

inline core::Result<model::PostPayload, PostCodecError>
decode_post(const Bytes& bytes) {
    return decode_post(bytes.data(), bytes.size());
}

MessageTag message_tag(const core::MessageId& message_id) noexcept;

} // namespace lora::protocol
