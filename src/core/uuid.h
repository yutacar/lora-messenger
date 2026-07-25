/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lora::ports {
class IRandomBytes;
}

namespace lora::core {

class Uuid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    Uuid() noexcept = default;

    static Uuid from_bytes(Bytes bytes) noexcept;
    static std::optional<Uuid> parse(std::string_view value) noexcept;
    static std::optional<Uuid> generate_v4(ports::IRandomBytes& random) noexcept;

    const Bytes& bytes() const noexcept;
    std::string to_string() const;
    bool is_nil() const noexcept;
    bool is_v4() const noexcept;

    friend bool operator==(const Uuid& left, const Uuid& right) noexcept;
    friend bool operator!=(const Uuid& left, const Uuid& right) noexcept;
    friend bool operator<(const Uuid& left, const Uuid& right) noexcept;

private:
    explicit Uuid(Bytes bytes) noexcept;

    Bytes bytes_{};
};

struct UuidHash {
    std::size_t operator()(const Uuid& value) const noexcept;
};

enum class IdError {
    InvalidUuidV4,
    RandomUnavailable,
};

class InstallId {
public:
    static Result<InstallId, IdError> from_uuid(Uuid value);
    static Result<InstallId, IdError> generate(ports::IRandomBytes& random);

    const Uuid& uuid() const noexcept;
    std::string to_string() const;

    friend bool operator==(const InstallId& left, const InstallId& right) noexcept;
    friend bool operator!=(const InstallId& left, const InstallId& right) noexcept;
    friend bool operator<(const InstallId& left, const InstallId& right) noexcept;

private:
    explicit InstallId(Uuid value) noexcept;
    Uuid value_;
};

class MessageId {
public:
    static Result<MessageId, IdError> from_uuid(Uuid value);
    static Result<MessageId, IdError> generate(ports::IRandomBytes& random);

    const Uuid& uuid() const noexcept;
    std::string to_string() const;

    friend bool operator==(const MessageId& left, const MessageId& right) noexcept;
    friend bool operator!=(const MessageId& left, const MessageId& right) noexcept;
    friend bool operator<(const MessageId& left, const MessageId& right) noexcept;

private:
    explicit MessageId(Uuid value) noexcept;
    Uuid value_;
};

} // namespace lora::core
