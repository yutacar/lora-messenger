/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/settings_json.h"

#include "core/text.h"
#include "core/uuid.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace lora::adapters::storage {
namespace {

constexpr std::size_t kMaximumJsonNesting = 12U;

struct JsonValue {
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Object,
        Array,
    };

    Type type{Type::Null};
    std::string scalar;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) noexcept : input_(input) {}

    bool parse(JsonValue& output) {
        skip_whitespace();
        if (!parse_value(output, 0U)) {
            return false;
        }
        skip_whitespace();
        if (position_ != input_.size()) {
            error_ = SettingsJsonError::MalformedJson;
            return false;
        }
        return true;
    }

    SettingsJsonError error() const noexcept {
        return error_;
    }

private:
    bool parse_value(JsonValue& output, std::size_t depth) {
        if (position_ >= input_.size()) {
            return malformed();
        }

        const char token = input_[position_];
        if (token == '{') {
            if (depth >= kMaximumJsonNesting) {
                error_ = SettingsJsonError::ExcessiveNesting;
                return false;
            }
            return parse_object(output, depth + 1U);
        }
        if (token == '[') {
            if (depth >= kMaximumJsonNesting) {
                error_ = SettingsJsonError::ExcessiveNesting;
                return false;
            }
            return parse_array(output, depth + 1U);
        }
        if (token == '"') {
            output.type = JsonValue::Type::String;
            return parse_string(output.scalar);
        }
        if (token == 't') {
            output.type = JsonValue::Type::Boolean;
            output.scalar = "true";
            return consume_literal("true");
        }
        if (token == 'f') {
            output.type = JsonValue::Type::Boolean;
            output.scalar = "false";
            return consume_literal("false");
        }
        if (token == 'n') {
            output.type = JsonValue::Type::Null;
            return consume_literal("null");
        }
        if (token == '-' || (token >= '0' && token <= '9')) {
            output.type = JsonValue::Type::Number;
            return parse_number(output.scalar);
        }
        return malformed();
    }

    bool parse_object(JsonValue& output, std::size_t depth) {
        output.type = JsonValue::Type::Object;
        ++position_;
        skip_whitespace();
        if (consume('}')) {
            return true;
        }

        while (position_ < input_.size()) {
            if (input_[position_] != '"') {
                return malformed();
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            for (const auto& existing : output.object) {
                if (existing.first == key) {
                    error_ = SettingsJsonError::DuplicateKey;
                    return false;
                }
            }

            skip_whitespace();
            if (!consume(':')) {
                return malformed();
            }
            skip_whitespace();

            JsonValue value;
            if (!parse_value(value, depth)) {
                return false;
            }
            output.object.emplace_back(std::move(key), std::move(value));

            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return malformed();
            }
            skip_whitespace();
        }
        return malformed();
    }

    bool parse_array(JsonValue& output, std::size_t depth) {
        output.type = JsonValue::Type::Array;
        ++position_;
        skip_whitespace();
        if (consume(']')) {
            return true;
        }

        while (position_ < input_.size()) {
            JsonValue value;
            if (!parse_value(value, depth)) {
                return false;
            }
            output.array.emplace_back(std::move(value));
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return malformed();
            }
            skip_whitespace();
        }
        return malformed();
    }

    bool parse_string(std::string& output) {
        if (!consume('"')) {
            return malformed();
        }

        while (position_ < input_.size()) {
            const auto value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == static_cast<unsigned char>('"')) {
                return true;
            }
            if (value < 0x20U) {
                return malformed();
            }
            if (value != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(value));
                continue;
            }

            if (position_ >= input_.size()) {
                return malformed();
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!parse_hex_quad(codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (position_ + 2U > input_.size() ||
                            input_[position_] != '\\' ||
                            input_[position_ + 1U] != 'u') {
                            return malformed();
                        }
                        position_ += 2U;
                        std::uint32_t low = 0;
                        if (!parse_hex_quad(low)) {
                            return false;
                        }
                        if (low < 0xdc00U || low > 0xdfffU) {
                            return malformed();
                        }
                        codepoint = 0x10000U +
                            ((codepoint - 0xd800U) << 10U) +
                            (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U &&
                               codepoint <= 0xdfffU) {
                        return malformed();
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: return malformed();
            }
        }
        return malformed();
    }

    bool parse_hex_quad(std::uint32_t& output) {
        if (position_ + 4U > input_.size()) {
            return malformed();
        }
        output = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            const int value = hex_value(input_[position_++]);
            if (value < 0) {
                return malformed();
            }
            output = (output << 4U) | static_cast<std::uint32_t>(value);
        }
        return true;
    }

    bool parse_number(std::string& output) {
        const std::size_t begin = position_;
        if (consume('-') && position_ >= input_.size()) {
            return malformed();
        }
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return malformed();
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' ||
                input_[position_] > '9') {
                return malformed();
            }
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                return malformed();
            }
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                return malformed();
            }
            while (position_ < input_.size() &&
                   input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        output.assign(input_.substr(begin, position_ - begin));
        return true;
    }

    bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return malformed();
        }
        position_ += literal.size();
        return true;
    }

    bool consume(char expected) noexcept {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_whitespace() noexcept {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' &&
                value != '\n' && value != '\r') {
                return;
            }
            ++position_;
        }
    }

    bool malformed() noexcept {
        error_ = SettingsJsonError::MalformedJson;
        return false;
    }

    static int hex_value(char value) noexcept {
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

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    std::string_view input_;
    std::size_t position_{0};
    SettingsJsonError error_{SettingsJsonError::MalformedJson};
};

const JsonValue* member(const JsonValue& object, std::string_view key) {
    for (const auto& entry : object.object) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool has_only_keys(const JsonValue& object,
                   const std::vector<std::string_view>& allowed) {
    for (const auto& entry : object.object) {
        bool known = false;
        for (const auto candidate : allowed) {
            if (entry.first == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

bool parse_canonical_uint64(std::string_view value,
                            std::uint64_t& output) noexcept {
    if (value.empty() || (value.size() > 1U && value.front() == '0')) {
        return false;
    }
    std::uint64_t result = 0;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (maximum - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    output = result;
    return true;
}

void append_json_string(std::string& output, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '"': output.append("\\\""); break;
            case '\\': output.append("\\\\"); break;
            case '\b': output.append("\\b"); break;
            case '\f': output.append("\\f"); break;
            case '\n': output.append("\\n"); break;
            case '\r': output.append("\\r"); break;
            case '\t': output.append("\\t"); break;
            default:
                if (character < 0x20U) {
                    output.append("\\u00");
                    output.push_back(kHex[(character >> 4U) & 0x0fU]);
                    output.push_back(kHex[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output.push_back('"');
}

core::Result<persistence::SettingsRecord, SettingsJsonError>
failure(SettingsJsonError error) {
    return core::Result<persistence::SettingsRecord,
                        SettingsJsonError>::failure(error);
}

} // namespace

core::Result<persistence::SettingsRecord, SettingsJsonError>
parse_settings_json(std::string_view json) {
    if (json.size() > kMaxSettingsJsonBytes) {
        return failure(SettingsJsonError::TooLarge);
    }
    if (!core::is_valid_utf8(json)) {
        return failure(SettingsJsonError::InvalidUtf8);
    }

    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root)) {
        return failure(parser.error());
    }
    if (root.type != JsonValue::Type::Object) {
        return failure(SettingsJsonError::WrongType);
    }
    if (!has_only_keys(root, {"format", "schema_version", "generation",
                              "identity", "ui", "history"})) {
        return failure(SettingsJsonError::UnknownKey);
    }

    const auto* format = member(root, "format");
    const auto* schema_version = member(root, "schema_version");
    const auto* generation = member(root, "generation");
    const auto* identity = member(root, "identity");
    const auto* ui = member(root, "ui");
    const auto* history = member(root, "history");
    if (format == nullptr || schema_version == nullptr ||
        generation == nullptr || identity == nullptr || ui == nullptr ||
        history == nullptr) {
        return failure(SettingsJsonError::MissingKey);
    }
    if (format->type != JsonValue::Type::String ||
        schema_version->type != JsonValue::Type::Number ||
        generation->type != JsonValue::Type::String ||
        identity->type != JsonValue::Type::Object ||
        ui->type != JsonValue::Type::Object ||
        history->type != JsonValue::Type::Object) {
        return failure(SettingsJsonError::WrongType);
    }
    if (format->scalar != kSettingsFormat) {
        return failure(SettingsJsonError::UnsupportedFormat);
    }
    const bool schema_v1 = schema_version->scalar == "1";
    const bool schema_v2 = schema_version->scalar == "2";
    if (!schema_v1 && !schema_v2) {
        return failure(SettingsJsonError::UnsupportedSchemaVersion);
    }

    std::uint64_t parsed_generation = 0;
    if (!parse_canonical_uint64(generation->scalar, parsed_generation) ||
        parsed_generation == 0U) {
        return failure(SettingsJsonError::InvalidGeneration);
    }

    if (!has_only_keys(*identity,
                       {"install_uuid", "user_id",
                        "sender_sequence_high_watermark"})) {
        return failure(SettingsJsonError::UnknownKey);
    }
    const auto* install_uuid = member(*identity, "install_uuid");
    const auto* user_id = member(*identity, "user_id");
    const auto* sender_sequence =
        member(*identity, "sender_sequence_high_watermark");
    if (install_uuid == nullptr || user_id == nullptr ||
        sender_sequence == nullptr) {
        return failure(SettingsJsonError::MissingKey);
    }
    if (install_uuid->type != JsonValue::Type::String ||
        user_id->type != JsonValue::Type::String ||
        sender_sequence->type != JsonValue::Type::String) {
        return failure(SettingsJsonError::WrongType);
    }

    const auto uuid = core::Uuid::parse(install_uuid->scalar);
    if (!uuid || uuid->to_string() != install_uuid->scalar ||
        !uuid->is_v4()) {
        return failure(SettingsJsonError::InvalidInstallUuid);
    }
    auto install_id = core::InstallId::from_uuid(*uuid);
    if (!install_id) {
        return failure(SettingsJsonError::InvalidInstallUuid);
    }
    auto parsed_user_id = core::UserId::create(user_id->scalar);
    if (!parsed_user_id) {
        return failure(SettingsJsonError::InvalidUserId);
    }
    std::uint64_t parsed_sender_sequence = 0;
    if (!parse_canonical_uint64(sender_sequence->scalar,
                                parsed_sender_sequence)) {
        return failure(SettingsJsonError::InvalidSenderSequence);
    }

    if ((schema_v1 && !has_only_keys(*ui, {"locale"})) ||
        (schema_v2 &&
         !has_only_keys(*ui, {"locale", "skip_title"}))) {
        return failure(SettingsJsonError::UnknownKey);
    }
    const auto* locale = member(*ui, "locale");
    const auto* skip_title = member(*ui, "skip_title");
    if (locale == nullptr || (schema_v2 && skip_title == nullptr)) {
        return failure(SettingsJsonError::MissingKey);
    }
    if (locale->type != JsonValue::Type::String ||
        (schema_v2 &&
         skip_title->type != JsonValue::Type::Boolean)) {
        return failure(SettingsJsonError::WrongType);
    }

    persistence::StoredLocale stored_locale;
    if (locale->scalar == "en") {
        stored_locale = persistence::StoredLocale::English;
    } else if (locale->scalar == "ja") {
        stored_locale = persistence::StoredLocale::Japanese;
    } else if (locale->scalar == "zh-Hans") {
        stored_locale = persistence::StoredLocale::SimplifiedChinese;
    } else {
        return failure(SettingsJsonError::InvalidLocale);
    }
    const bool stored_skip_title =
        schema_v2 && skip_title->scalar == "true";

    if (!has_only_keys(*history, {"initialized"})) {
        return failure(SettingsJsonError::UnknownKey);
    }
    const auto* initialized = member(*history, "initialized");
    if (initialized == nullptr) {
        return failure(SettingsJsonError::MissingKey);
    }
    if (initialized->type != JsonValue::Type::Boolean) {
        return failure(SettingsJsonError::WrongType);
    }
    const bool history_initialized = initialized->scalar == "true";

    return core::Result<persistence::SettingsRecord,
                        SettingsJsonError>::success(
        persistence::SettingsRecord{
            parsed_generation,
            std::move(install_id).value(),
            std::move(parsed_user_id).value(),
            parsed_sender_sequence,
            stored_locale,
            history_initialized,
            stored_skip_title,
        });
}

core::Result<std::string, SettingsJsonError>
serialize_settings_json(const persistence::SettingsRecord& settings) {
    if (settings.generation == 0U) {
        return core::Result<std::string, SettingsJsonError>::failure(
            SettingsJsonError::InvalidGeneration);
    }
    const auto locale = persistence::stored_locale_code(settings.locale);
    if (locale.empty()) {
        return core::Result<std::string, SettingsJsonError>::failure(
            SettingsJsonError::InvalidLocale);
    }

    std::string output;
    output.reserve(256U);
    output.append("{\"format\":");
    append_json_string(output, kSettingsFormat);
    output.append(",\"schema_version\":2,\"generation\":");
    append_json_string(output, std::to_string(settings.generation));
    output.append(",\"identity\":{\"install_uuid\":");
    append_json_string(output, settings.install_id.to_string());
    output.append(",\"user_id\":");
    append_json_string(output, settings.user_id.value());
    output.append(",\"sender_sequence_high_watermark\":");
    append_json_string(
        output, std::to_string(settings.sender_sequence_high_watermark));
    output.append("},\"ui\":{\"locale\":");
    append_json_string(output, locale);
    output.append(",\"skip_title\":");
    output.append(settings.skip_title ? "true" : "false");
    output.append("},\"history\":{\"initialized\":");
    output.append(settings.history_initialized ? "true" : "false");
    output.append("}}\n");

    if (output.size() > kMaxSettingsJsonBytes) {
        return core::Result<std::string, SettingsJsonError>::failure(
            SettingsJsonError::TooLarge);
    }
    return core::Result<std::string, SettingsJsonError>::success(
        std::move(output));
}

} // namespace lora::adapters::storage
