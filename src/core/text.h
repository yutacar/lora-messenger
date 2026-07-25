/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"

#include <string>
#include <string_view>

namespace lora::core {

enum class TextError {
    None,
    Empty,
    TooLong,
    InvalidUtf8,
    ForbiddenCharacter,
    EdgeWhitespace,
};

bool is_valid_utf8(std::string_view value) noexcept;

class UserId {
public:
    static Result<UserId, TextError> create(std::string_view value);

    const std::string& value() const noexcept;

    friend bool operator==(const UserId& left, const UserId& right) noexcept;
    friend bool operator!=(const UserId& left, const UserId& right) noexcept;

private:
    explicit UserId(std::string value);
    std::string value_;
};

class PostBody {
public:
    static Result<PostBody, TextError> create(std::string_view value);

    const std::string& value() const noexcept;

    friend bool operator==(const PostBody& left, const PostBody& right) noexcept;
    friend bool operator!=(const PostBody& left, const PostBody& right) noexcept;

private:
    explicit PostBody(std::string value);
    std::string value_;
};

} // namespace lora::core
