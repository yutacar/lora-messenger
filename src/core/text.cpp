/*
 * SPDX-License-Identifier: MIT
 */

#include "core/text.h"

#include "core/limits.h"

#include <cstdint>
#include <utility>

namespace lora::core {
namespace {

template<typename Visitor>
bool visit_utf8(std::string_view value, Visitor&& visitor) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        std::uint32_t codepoint = 0;
        std::size_t count = 0;

        if (first <= 0x7fU) {
            codepoint = first;
            count = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            codepoint = first & 0x1fU;
            count = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codepoint = first & 0x0fU;
            count = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            codepoint = first & 0x07U;
            count = 4;
        } else {
            return false;
        }

        if (index + count > value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }

        if ((count == 2 && codepoint < 0x80U) ||
            (count == 3 && codepoint < 0x800U) ||
            (count == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            return false;
        }

        visitor(codepoint);
        index += count;
    }
    return true;
}

bool is_whitespace(std::uint32_t codepoint) noexcept {
    switch (codepoint) {
        case 0x0009:
        case 0x000a:
        case 0x000b:
        case 0x000c:
        case 0x000d:
        case 0x0020:
        case 0x0085:
        case 0x00a0:
        case 0x1680:
        case 0x2000:
        case 0x2001:
        case 0x2002:
        case 0x2003:
        case 0x2004:
        case 0x2005:
        case 0x2006:
        case 0x2007:
        case 0x2008:
        case 0x2009:
        case 0x200a:
        case 0x2028:
        case 0x2029:
        case 0x202f:
        case 0x205f:
        case 0x3000:
            return true;
        default:
            return false;
    }
}

bool is_bidi_format(std::uint32_t codepoint) noexcept {
    return codepoint == 0x061cU || codepoint == 0x200eU || codepoint == 0x200fU ||
           (codepoint >= 0x202aU && codepoint <= 0x202eU) ||
           (codepoint >= 0x2066U && codepoint <= 0x2069U);
}

bool is_unicode_line_separator(std::uint32_t codepoint) noexcept {
    return codepoint == 0x2028U || codepoint == 0x2029U;
}

bool is_noncharacter(std::uint32_t codepoint) noexcept {
    return (codepoint >= 0xfdd0U && codepoint <= 0xfdefU) ||
           (codepoint & 0xffffU) == 0xfffeU || (codepoint & 0xffffU) == 0xffffU;
}

bool is_control(std::uint32_t codepoint) noexcept {
    return codepoint <= 0x1fU || (codepoint >= 0x7fU && codepoint <= 0x9fU);
}

TextError validate_text(std::string_view value, std::size_t maximum_bytes,
                        bool user_id) noexcept {
    if (value.empty()) {
        return TextError::Empty;
    }
    if (value.size() > maximum_bytes) {
        return TextError::TooLong;
    }

    bool forbidden = false;
    bool has_non_whitespace = false;
    bool has_codepoint = false;
    std::uint32_t first_codepoint = 0;
    std::uint32_t last_codepoint = 0;
    const bool valid = visit_utf8(value, [&](std::uint32_t codepoint) {
        if (!has_codepoint) {
            first_codepoint = codepoint;
            has_codepoint = true;
        }
        last_codepoint = codepoint;
        has_non_whitespace = has_non_whitespace || !is_whitespace(codepoint);

        const bool allowed_line_feed = !user_id && codepoint == 0x000aU;
        if ((is_control(codepoint) && !allowed_line_feed) ||
            is_unicode_line_separator(codepoint) || is_bidi_format(codepoint) ||
            is_noncharacter(codepoint)) {
            forbidden = true;
        }
    });

    if (!valid) {
        return TextError::InvalidUtf8;
    }
    if (forbidden) {
        return TextError::ForbiddenCharacter;
    }
    if (!has_non_whitespace) {
        return TextError::Empty;
    }
    if (user_id && (is_whitespace(first_codepoint) || is_whitespace(last_codepoint))) {
        return TextError::EdgeWhitespace;
    }
    return TextError::None;
}

bool is_success(TextError error) noexcept {
    return error == TextError::None;
}

} // namespace

bool is_valid_utf8(std::string_view value) noexcept {
    return visit_utf8(value, [](std::uint32_t) {});
}

UserId::UserId(std::string value) : value_(std::move(value)) {}

Result<UserId, TextError> UserId::create(std::string_view value) {
    const auto error = validate_text(value, kMaxUserIdBytes, true);
    if (!is_success(error)) {
        return Result<UserId, TextError>::failure(error);
    }
    return Result<UserId, TextError>::success(UserId(std::string(value)));
}

const std::string& UserId::value() const noexcept { return value_; }
bool operator==(const UserId& left, const UserId& right) noexcept { return left.value_ == right.value_; }
bool operator!=(const UserId& left, const UserId& right) noexcept { return !(left == right); }

PostBody::PostBody(std::string value) : value_(std::move(value)) {}

Result<PostBody, TextError> PostBody::create(std::string_view value) {
    const auto error = validate_text(value, kMaxPostBodyBytes, false);
    if (!is_success(error)) {
        return Result<PostBody, TextError>::failure(error);
    }
    return Result<PostBody, TextError>::success(PostBody(std::string(value)));
}

const std::string& PostBody::value() const noexcept { return value_; }
bool operator==(const PostBody& left, const PostBody& right) noexcept { return left.value_ == right.value_; }
bool operator!=(const PostBody& left, const PostBody& right) noexcept { return !(left == right); }

} // namespace lora::core
