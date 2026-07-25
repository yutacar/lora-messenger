/*
 * SPDX-License-Identifier: MIT
 */

#include "app_script_parser.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace platform::script {
namespace {

bool is_ascii_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

char ascii_upper(char character) noexcept {
    if (character >= 'a' && character <= 'z') {
        return static_cast<char>(character - ('a' - 'A'));
    }
    return character;
}

bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char lhs, char rhs) {
                          return ascii_upper(lhs) == ascii_upper(rhs);
                      });
}

bool ascii_starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           ascii_equal(value.substr(0, prefix.size()), prefix);
}

ParseResult failure(ParseError error, std::size_t token_index,
                    std::string diagnostic) {
    ParseResult result;
    result.error = error;
    result.token_index = token_index;
    result.diagnostic = std::move(diagnostic);
    return result;
}

bool append_action(ParseResult& result, Action action, std::size_t token_index) {
    if (result.actions.size() >= kMaximumActions) {
        result = failure(ParseError::TooManyActions, token_index,
                         "expanded action count exceeds 1024");
        return false;
    }
    result.actions.push_back(std::move(action));
    return true;
}

bool parse_decimal(std::string_view value, std::uint32_t& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    std::uint32_t accumulator = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(character - '0');
        if (accumulator > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            return false;
        }
        accumulator = accumulator * 10U + digit;
    }
    parsed = accumulator;
    return true;
}

bool is_unreserved(char character) noexcept {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '.' || character == '_' || character == '~';
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + character - 'a';
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + character - 'A';
    }
    return -1;
}

bool percent_decode(std::string_view encoded, std::string& decoded,
                    std::string& diagnostic) {
    if (encoded.empty()) {
        diagnostic = "TEXT value must not be empty";
        return false;
    }
    decoded.clear();
    decoded.reserve(std::min(encoded.size(), kMaximumTextBytes));
    for (std::size_t index = 0; index < encoded.size();) {
        unsigned char byte = 0;
        if (encoded[index] == '%') {
            if (index + 2U >= encoded.size()) {
                diagnostic = "TEXT contains an incomplete percent escape";
                return false;
            }
            const int high = hex_value(encoded[index + 1U]);
            const int low = hex_value(encoded[index + 2U]);
            if (high < 0 || low < 0) {
                diagnostic = "TEXT contains a non-hex percent escape";
                return false;
            }
            byte = static_cast<unsigned char>((high << 4) | low);
            index += 3U;
        } else {
            const auto raw = static_cast<unsigned char>(encoded[index]);
            if (raw > 0x7fU || !is_unreserved(encoded[index])) {
                diagnostic = "TEXT reserved and non-ASCII bytes must use percent escapes";
                return false;
            }
            byte = raw;
            ++index;
        }
        if (decoded.size() >= kMaximumTextBytes) {
            diagnostic = "TEXT exceeds 160 decoded UTF-8 bytes";
            return false;
        }
        decoded.push_back(static_cast<char>(byte));
    }
    return true;
}

bool is_bidi_format(std::uint32_t codepoint) noexcept {
    return codepoint == 0x061cU || codepoint == 0x200eU || codepoint == 0x200fU ||
           (codepoint >= 0x202aU && codepoint <= 0x202eU) ||
           (codepoint >= 0x2066U && codepoint <= 0x2069U);
}

bool is_noncharacter(std::uint32_t codepoint) noexcept {
    return (codepoint >= 0xfdd0U && codepoint <= 0xfdefU) ||
           (codepoint & 0xffffU) == 0xfffeU || (codepoint & 0xffffU) == 0xffffU;
}

bool is_unsafe_text_scalar(std::uint32_t codepoint) noexcept {
    return codepoint <= 0x1fU || (codepoint >= 0x7fU && codepoint <= 0x9fU) ||
           codepoint == 0x2028U || codepoint == 0x2029U ||
           is_bidi_format(codepoint) || is_noncharacter(codepoint);
}

bool decode_utf8_scalars(std::string_view value, std::vector<std::uint32_t>& scalars,
                         std::string& diagnostic) {
    scalars.clear();
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
            diagnostic = "TEXT is not valid UTF-8";
            return false;
        }

        if (count > value.size() - index) {
            diagnostic = "TEXT is not valid UTF-8";
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                diagnostic = "TEXT is not valid UTF-8";
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }

        if ((count == 2U && codepoint < 0x80U) ||
            (count == 3U && codepoint < 0x800U) ||
            (count == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            diagnostic = "TEXT is not valid scalar UTF-8";
            return false;
        }
        if (is_unsafe_text_scalar(codepoint)) {
            diagnostic = "TEXT contains a forbidden control or format scalar";
            return false;
        }
        scalars.push_back(codepoint);
        index += count;
    }
    return !scalars.empty();
}

bool is_stable_atom(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum || value.front() < 'a' ||
        value.front() > 'z') {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool is_stable_value(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum ||
        !((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9'))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == '-';
    });
}

bool is_probe_field(std::string_view field) noexcept {
    static constexpr std::array<std::string_view, 8> fields{
        "screen", "modal", "locale", "focus", "status",
        "count", "persistence", "newest-state",
    };
    return std::find(fields.begin(), fields.end(), field) != fields.end();
}

bool parse_probe(std::string_view payload, std::string& field, std::string& value,
                 std::string& diagnostic) {
    const auto separator = payload.find(':');
    if (separator == std::string_view::npos ||
        payload.find(':', separator + 1U) != std::string_view::npos) {
        diagnostic = "expectation must use field:value";
        return false;
    }
    const auto field_view = payload.substr(0, separator);
    const auto value_view = payload.substr(separator + 1U);
    if (!is_stable_atom(field_view, 16U) || !is_probe_field(field_view)) {
        diagnostic = "expectation uses an unsupported field";
        return false;
    }
    if (!is_stable_value(value_view, 64U)) {
        diagnostic = "expectation value is not a stable lowercase ASCII id";
        return false;
    }
    field.assign(field_view);
    value.assign(value_view);
    return true;
}

NamedKey named_key_for(std::string_view token) noexcept {
    if (ascii_equal(token, "HOME")) return NamedKey::Home;
    if (ascii_equal(token, "ESC")) return NamedKey::Escape;
    if (ascii_equal(token, "UP")) return NamedKey::Up;
    if (ascii_equal(token, "DOWN")) return NamedKey::Down;
    if (ascii_equal(token, "LEFT")) return NamedKey::Left;
    if (ascii_equal(token, "RIGHT")) return NamedKey::Right;
    if (ascii_equal(token, "ENTER")) return NamedKey::Enter;
    if (ascii_equal(token, "BACKSPACE")) return NamedKey::Backspace;
    if (ascii_equal(token, "TAB")) return NamedKey::Tab;
    return NamedKey::None;
}

} // namespace

bool is_safe_screenshot_stem(std::string_view stem) noexcept {
    if (stem.empty() || stem.size() > 64U) {
        return false;
    }
    const auto is_alphanumeric = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!is_alphanumeric(stem.front()) || !is_alphanumeric(stem.back())) {
        return false;
    }
    return std::all_of(stem.begin(), stem.end(), [&](char character) {
        return is_alphanumeric(character) || character == '.' || character == '_' ||
               character == '-';
    });
}

ParseResult parse(std::string_view source) {
    if (source.size() > kMaximumSourceBytes) {
        return failure(ParseError::SourceTooLong, 0,
                       "script source exceeds 16384 bytes");
    }

    ParseResult result;
    if (source.empty()) {
        return result;
    }

    std::size_t begin = 0;
    std::size_t token_index = 0;
    while (begin <= source.size()) {
        ++token_index;
        const auto end = source.find(',', begin);
        const auto length = end == std::string_view::npos ? source.size() - begin
                                                          : end - begin;
        const auto token = trim(source.substr(begin, length));
        if (token.empty()) {
            return failure(ParseError::EmptyToken, token_index,
                           "empty script token is not allowed");
        }
        if (token.size() > kMaximumTokenBytes) {
            return failure(ParseError::TokenTooLong, token_index,
                           "script token exceeds 512 bytes");
        }

        const auto key = named_key_for(token);
        if (key != NamedKey::None) {
            if (!append_action(result, {ActionType::Key, key, 0, {}, {}}, token_index)) {
                return result;
            }
        } else if (ascii_equal(token, "N") || ascii_equal(token, "R") ||
                   ascii_equal(token, "M") || ascii_equal(token, "S") ||
                   ascii_equal(token, "D")) {
            const auto scalar = static_cast<std::uint32_t>(
                ascii_upper(token.front()) - 'A' + 'a');
            if (!append_action(result,
                               {ActionType::Key, NamedKey::None, scalar, {}, {}},
                               token_index)) {
                return result;
            }
        } else if (ascii_equal(token, "WAIT")) {
            if (!append_action(result, {ActionType::Wait, NamedKey::None, 0, {}, {}},
                               token_index)) {
                return result;
            }
        } else if (ascii_starts_with(token, "WAIT=")) {
            std::uint32_t count = 0;
            if (!parse_decimal(token.substr(5), count) || count == 0U ||
                count > kMaximumWaitTicks) {
                return failure(ParseError::InvalidValue, token_index,
                               "WAIT count must be an integer from 1 through 100");
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                if (!append_action(result,
                                   {ActionType::Wait, NamedKey::None, 0, {}, {}},
                                   token_index)) {
                    return result;
                }
            }
        } else if (ascii_starts_with(token, "TEXT=")) {
            std::string decoded;
            std::string diagnostic;
            if (!percent_decode(token.substr(5), decoded, diagnostic)) {
                return failure(ParseError::InvalidTextEncoding, token_index,
                               std::move(diagnostic));
            }
            std::vector<std::uint32_t> scalars;
            if (!decode_utf8_scalars(decoded, scalars, diagnostic)) {
                return failure(ParseError::UnsafeText, token_index,
                               std::move(diagnostic));
            }
            for (const auto scalar : scalars) {
                if (!append_action(result,
                                   {ActionType::Key, NamedKey::None, scalar, {}, {}},
                                   token_index)) {
                    return result;
                }
            }
        } else if (ascii_starts_with(token, "SHOT=")) {
            const auto stem = token.substr(5);
            if (!stem.empty() && !is_safe_screenshot_stem(stem)) {
                return failure(ParseError::InvalidValue, token_index,
                               "SHOT stem is not a canonical safe lowercase name");
            }
            if (!append_action(result,
                               {ActionType::Screenshot, NamedKey::None, 0, {},
                                std::string(stem)},
                               token_index)) {
                return result;
            }
        } else if (ascii_starts_with(token, "EXPECT=") ||
                   ascii_starts_with(token, "AWAIT=")) {
            const bool await = ascii_starts_with(token, "AWAIT=");
            const std::size_t prefix_size = await ? 6U : 7U;
            std::string field;
            std::string value;
            std::string diagnostic;
            if (!parse_probe(token.substr(prefix_size), field, value, diagnostic)) {
                return failure(ParseError::InvalidValue, token_index,
                               std::move(diagnostic));
            }
            if (!append_action(result,
                               {await ? ActionType::Await : ActionType::Expect,
                                NamedKey::None, 0, std::move(field), std::move(value)},
                               token_index)) {
                return result;
            }
        } else if (ascii_equal(token, "CLOSE")) {
            if (end != std::string_view::npos) {
                return failure(ParseError::CloseMustBeLast, token_index,
                               "CLOSE must be the final script token");
            }
            if (!append_action(result,
                               {ActionType::CloseWindow, NamedKey::None, 0, {}, {}},
                               token_index)) {
                return result;
            }
        } else {
            return failure(ParseError::UnknownToken, token_index,
                           "unknown script token");
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

IntervalResult parse_interval(const char* configured) {
    if (!configured) {
        return {};
    }
    std::size_t length = 0;
    while (length <= 10U && configured[length] != '\0') {
        ++length;
    }
    if (length > 10U) {
        return {kDefaultIntervalMilliseconds, false,
                "APP_SCRIPT_INTERVAL_MS must be an integer from 20 through 5000"};
    }
    const std::string_view value{configured, length};
    std::uint32_t parsed = 0;
    if (!parse_decimal(value, parsed) || parsed < kMinimumIntervalMilliseconds ||
        parsed > kMaximumIntervalMilliseconds) {
        return {kDefaultIntervalMilliseconds, false,
                "APP_SCRIPT_INTERVAL_MS must be an integer from 20 through 5000"};
    }
    return {parsed, true, {}};
}

} // namespace platform::script
