/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"
#include "model/post.h"
#include "protocol/limits.h"

#include <cstddef>
#include <vector>

namespace lora::protocol {

enum class FragmentError {
    None,
    InvalidMtu,
    PostEncodeFailed,
    TooManyFragments,
    FrameEncodeFailed,
};

core::Result<std::vector<Bytes>, FragmentError>
fragment_post(const model::PostPayload& post, std::size_t mtu);

} // namespace lora::protocol
