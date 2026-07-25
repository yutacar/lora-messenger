/*
 * SPDX-License-Identifier: MIT
 */

#include "core/uuid.h"

#include "ports/random.h"

#include <algorithm>

namespace lora::core {
namespace {

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

Uuid::Uuid(Bytes bytes) noexcept : bytes_(bytes) {}

Uuid Uuid::from_bytes(Bytes bytes) noexcept {
    return Uuid(bytes);
}

std::optional<Uuid> Uuid::parse(std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return std::nullopt;
    }

    Bytes bytes{};
    std::size_t nibble_index = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const int nibble = hex_value(value[index]);
        if (nibble < 0) {
            return std::nullopt;
        }
        const std::size_t byte_index = nibble_index / 2;
        if ((nibble_index % 2) == 0) {
            bytes[byte_index] = static_cast<std::uint8_t>(nibble << 4);
        } else {
            bytes[byte_index] = static_cast<std::uint8_t>(bytes[byte_index] | nibble);
        }
        ++nibble_index;
    }
    if (nibble_index != 32) {
        return std::nullopt;
    }
    return Uuid(bytes);
}

std::optional<Uuid> Uuid::generate_v4(ports::IRandomBytes& random) noexcept {
    Bytes bytes{};
    if (!random.fill(bytes.data(), bytes.size())) {
        return std::nullopt;
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    return Uuid(bytes);
}

const Uuid::Bytes& Uuid::bytes() const noexcept {
    return bytes_;
}

std::string Uuid::to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(36);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output.push_back('-');
        }
        output.push_back(kHex[(bytes_[index] >> 4U) & 0x0fU]);
        output.push_back(kHex[bytes_[index] & 0x0fU]);
    }
    return output;
}

bool Uuid::is_nil() const noexcept {
    return std::all_of(bytes_.begin(), bytes_.end(),
                       [](std::uint8_t value) { return value == 0; });
}

bool Uuid::is_v4() const noexcept {
    return !is_nil() && (bytes_[6] & 0xf0U) == 0x40U &&
           (bytes_[8] & 0xc0U) == 0x80U;
}

bool operator==(const Uuid& left, const Uuid& right) noexcept {
    return left.bytes_ == right.bytes_;
}

bool operator!=(const Uuid& left, const Uuid& right) noexcept {
    return !(left == right);
}

bool operator<(const Uuid& left, const Uuid& right) noexcept {
    return left.bytes_ < right.bytes_;
}

std::size_t UuidHash::operator()(const Uuid& value) const noexcept {
    std::size_t hash = static_cast<std::size_t>(14695981039346656037ULL);
    for (const auto byte : value.bytes()) {
        hash ^= static_cast<std::size_t>(byte);
        hash *= static_cast<std::size_t>(1099511628211ULL);
    }
    return hash;
}

InstallId::InstallId(Uuid value) noexcept : value_(std::move(value)) {}

Result<InstallId, IdError> InstallId::from_uuid(Uuid value) {
    if (!value.is_v4()) {
        return Result<InstallId, IdError>::failure(IdError::InvalidUuidV4);
    }
    return Result<InstallId, IdError>::success(InstallId(std::move(value)));
}

Result<InstallId, IdError> InstallId::generate(ports::IRandomBytes& random) {
    auto value = Uuid::generate_v4(random);
    if (!value) {
        return Result<InstallId, IdError>::failure(IdError::RandomUnavailable);
    }
    return from_uuid(std::move(*value));
}

const Uuid& InstallId::uuid() const noexcept { return value_; }
std::string InstallId::to_string() const { return value_.to_string(); }
bool operator==(const InstallId& left, const InstallId& right) noexcept { return left.value_ == right.value_; }
bool operator!=(const InstallId& left, const InstallId& right) noexcept { return !(left == right); }
bool operator<(const InstallId& left, const InstallId& right) noexcept { return left.value_ < right.value_; }

MessageId::MessageId(Uuid value) noexcept : value_(std::move(value)) {}

Result<MessageId, IdError> MessageId::from_uuid(Uuid value) {
    if (!value.is_v4()) {
        return Result<MessageId, IdError>::failure(IdError::InvalidUuidV4);
    }
    return Result<MessageId, IdError>::success(MessageId(std::move(value)));
}

Result<MessageId, IdError> MessageId::generate(ports::IRandomBytes& random) {
    auto value = Uuid::generate_v4(random);
    if (!value) {
        return Result<MessageId, IdError>::failure(IdError::RandomUnavailable);
    }
    return from_uuid(std::move(*value));
}

const Uuid& MessageId::uuid() const noexcept { return value_; }
std::string MessageId::to_string() const { return value_.to_string(); }
bool operator==(const MessageId& left, const MessageId& right) noexcept { return left.value_ == right.value_; }
bool operator!=(const MessageId& left, const MessageId& right) noexcept { return !(left == right); }
bool operator<(const MessageId& left, const MessageId& right) noexcept { return left.value_ < right.value_; }

} // namespace lora::core
