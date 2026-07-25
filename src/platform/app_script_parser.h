/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platform::script {

inline constexpr std::size_t kMaximumSourceBytes = 16U * 1024U;
inline constexpr std::size_t kMaximumTokenBytes = 512U;
inline constexpr std::size_t kMaximumActions = 1024U;
inline constexpr std::size_t kMaximumTextBytes = 160U;
inline constexpr std::uint32_t kMinimumIntervalMilliseconds = 20U;
inline constexpr std::uint32_t kMaximumIntervalMilliseconds = 5000U;
inline constexpr std::uint32_t kDefaultIntervalMilliseconds = 200U;
inline constexpr std::uint32_t kMaximumWaitTicks = 100U;
inline constexpr std::uint32_t kMaximumAwaitTicks = 50U;

enum class ActionType {
    Key,
    Wait,
    Screenshot,
    Expect,
    Await,
    CloseWindow,
};

enum class NamedKey {
    None,
    Home,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Enter,
    Backspace,
    Tab,
};

struct Action {
    ActionType type{ActionType::Wait};
    NamedKey named_key{NamedKey::None};
    std::uint32_t scalar{0};
    std::string field;
    std::string value;
};

enum class ParseError {
    None,
    SourceTooLong,
    EmptyToken,
    TokenTooLong,
    UnknownToken,
    InvalidValue,
    InvalidTextEncoding,
    UnsafeText,
    TooManyActions,
    CloseMustBeLast,
};

struct ParseResult {
    std::vector<Action> actions;
    ParseError error{ParseError::None};
    std::size_t token_index{0};
    std::string diagnostic;

    explicit operator bool() const noexcept {
        return error == ParseError::None;
    }
};

struct IntervalResult {
    std::uint32_t milliseconds{kDefaultIntervalMilliseconds};
    bool valid{true};
    std::string diagnostic;
};

ParseResult parse(std::string_view source);
IntervalResult parse_interval(const char* configured);
bool is_safe_screenshot_stem(std::string_view stem) noexcept;

} // namespace platform::script
