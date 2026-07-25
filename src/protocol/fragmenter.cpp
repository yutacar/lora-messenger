/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/fragmenter.h"

#include "protocol/frame_codec.h"
#include "protocol/post_codec.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace lora::protocol {

core::Result<std::vector<Bytes>, FragmentError>
fragment_post(const model::PostPayload& post, std::size_t mtu) {
    if (!is_supported_mtu(mtu)) {
        return core::Result<std::vector<Bytes>, FragmentError>::failure(
            FragmentError::InvalidMtu);
    }

    auto encoded_post = encode_post(post);
    if (!encoded_post) {
        return core::Result<std::vector<Bytes>, FragmentError>::failure(
            FragmentError::PostEncodeFailed);
    }
    const Bytes& logical = encoded_post.value();
    const std::size_t capacity = frame_payload_capacity(mtu);
    const std::size_t fragment_count =
        (logical.size() + capacity - 1U) / capacity;
    if (fragment_count == 0U ||
        fragment_count > kMaximumFragments) {
        return core::Result<std::vector<Bytes>, FragmentError>::failure(
            FragmentError::TooManyFragments);
    }

    std::vector<Bytes> frames;
    frames.reserve(fragment_count);
    const MessageTag tag = message_tag(post.message_id());
    for (std::size_t index = 0; index < fragment_count; ++index) {
        const std::size_t offset = index * capacity;
        const std::size_t payload_size =
            std::min(capacity, logical.size() - offset);

        DataFrame frame;
        frame.tag = tag;
        frame.sender_sequence = post.sender_sequence();
        frame.fragment_index = static_cast<std::uint8_t>(index);
        frame.fragment_count =
            static_cast<std::uint8_t>(fragment_count);
        frame.total_length =
            static_cast<std::uint16_t>(logical.size());
        frame.payload.assign(
            logical.begin() + static_cast<std::ptrdiff_t>(offset),
            logical.begin() +
                static_cast<std::ptrdiff_t>(offset + payload_size));

        auto encoded_frame = encode_frame(frame, mtu);
        if (!encoded_frame) {
            return core::Result<std::vector<Bytes>, FragmentError>::failure(
                FragmentError::FrameEncodeFailed);
        }
        frames.push_back(std::move(encoded_frame).value());
    }
    return core::Result<std::vector<Bytes>, FragmentError>::success(
        std::move(frames));
}

} // namespace lora::protocol
