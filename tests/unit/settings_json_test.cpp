/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/storage/settings_json.h"

#include "test_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using lora::adapters::storage::SettingsJsonError;
using lora::persistence::SettingsRecord;
using lora::persistence::StoredLocale;

constexpr std::string_view kValidJson =
    "{\"format\":\"lora-messenger.settings\",\"schema_version\":1,"
    "\"generation\":\"1\",\"identity\":{\"install_uuid\":"
    "\"00112233-4455-4677-8899-aabbccddeeff\",\"user_id\":\"Mika\","
    "\"sender_sequence_high_watermark\":\"0\"},\"ui\":{\"locale\":\"en\"},"
    "\"history\":{\"initialized\":true}}";

std::string replace_once(std::string input, std::string_view from,
                         std::string_view to) {
    const auto position = input.find(from);
    if (position != std::string::npos) {
        input.replace(position, from.size(), to);
    }
    return input;
}

SettingsRecord make_record(std::uint64_t generation = 1U,
                           std::uint64_t sequence = 0U,
                           StoredLocale locale = StoredLocale::English,
                           std::string_view user = "Mika",
                           bool history_initialized = true,
                           bool skip_title = false) {
    auto user_id = lora::core::UserId::create(user);
    return SettingsRecord{
        generation,
        lora::test::make_install_id(0x10U),
        std::move(user_id).value(),
        sequence,
        locale,
        history_initialized,
        skip_title,
    };
}

} // namespace

int main() {
    using lora::adapters::storage::kMaxSettingsJsonBytes;
    using lora::adapters::storage::parse_settings_json;
    using lora::adapters::storage::serialize_settings_json;
    lora::test::Runner runner;

    runner.run("canonical serializer is stable and round trips all fields", [&] {
        const auto settings = make_record(
            42U, std::numeric_limits<std::uint64_t>::max(),
            StoredLocale::Japanese, "美香", true, true);
        const auto first = serialize_settings_json(settings);
        REQUIRE(first.has_value());
        const auto second = serialize_settings_json(settings);
        REQUIRE(second.has_value());
        CHECK_EQ(first.value(), second.value());
        CHECK_EQ(
            first.value(),
            "{\"format\":\"lora-messenger.settings\",\"schema_version\":2,"
            "\"generation\":\"42\",\"identity\":{\"install_uuid\":"
            "\"10111213-1415-4617-9819-1a1b1c1d1e1f\","
            "\"user_id\":\"美香\",\"sender_sequence_high_watermark\":"
            "\"18446744073709551615\"},\"ui\":{\"locale\":\"ja\","
            "\"skip_title\":true},"
            "\"history\":{\"initialized\":true}}\n");

        const auto parsed = parse_settings_json(first.value());
        REQUIRE(parsed.has_value());
        CHECK_EQ(parsed.value(), settings);
    });

    runner.run("parser accepts field reordering whitespace and unicode escapes", [&] {
        const auto parsed = parse_settings_json(
            " \n {\"history\":{\"initialized\":false},"
            "\"ui\":{\"locale\":\"zh-Hans\"},"
            "\"identity\":{\"user_id\":\"\\u7f8e\\u9999\","
            "\"sender_sequence_high_watermark\":\"7\","
            "\"install_uuid\":\"00112233-4455-4677-8899-aabbccddeeff\"},"
            "\"generation\":\"9\",\"schema_version\":1,"
            "\"format\":\"lora-messenger.settings\"}\t");
        REQUIRE(parsed.has_value());
        CHECK_EQ(parsed.value().generation, 9U);
        CHECK_EQ(parsed.value().sender_sequence_high_watermark, 7U);
        CHECK_EQ(parsed.value().user_id.value(), "美香");
        CHECK_EQ(parsed.value().locale, StoredLocale::SimplifiedChinese);
        CHECK(!parsed.value().history_initialized);
        CHECK(!parsed.value().skip_title);
    });

    runner.run("schema v1 migrates skip title off and v2 requires the flag", [&] {
        const auto migrated = parse_settings_json(kValidJson);
        REQUIRE(migrated.has_value());
        CHECK(!migrated.value().skip_title);

        const auto missing = parse_settings_json(replace_once(
            std::string(kValidJson), "\"schema_version\":1",
            "\"schema_version\":2"));
        CHECK(!missing.has_value());
        CHECK_EQ(missing.error(), SettingsJsonError::MissingKey);

        const auto wrong_type = parse_settings_json(replace_once(
            replace_once(
                std::string(kValidJson), "\"schema_version\":1",
                "\"schema_version\":2"),
            "\"locale\":\"en\"",
            "\"locale\":\"en\",\"skip_title\":\"true\""));
        CHECK(!wrong_type.has_value());
        CHECK_EQ(wrong_type.error(), SettingsJsonError::WrongType);
    });

    runner.run("history initialization marker round trips both booleans", [&] {
        for (const bool initialized : {false, true}) {
            const auto settings = make_record(
                3U, 2U, StoredLocale::English, "Mika", initialized);
            const auto serialized = serialize_settings_json(settings);
            REQUIRE(serialized.has_value());
            CHECK(serialized.value().find(
                      initialized ? "\"initialized\":true"
                                  : "\"initialized\":false") !=
                  std::string::npos);

            const auto parsed = parse_settings_json(serialized.value());
            REQUIRE(parsed.has_value());
            CHECK_EQ(parsed.value(), settings);
            CHECK_EQ(parsed.value().history_initialized, initialized);
        }
    });

    runner.run("all three supported locale codes are accepted", [&] {
        for (const auto code : {"en", "ja", "zh-Hans"}) {
            const auto json = replace_once(std::string(kValidJson),
                                           "\"locale\":\"en\"",
                                           std::string("\"locale\":\"") +
                                               code + "\"");
            CHECK(parse_settings_json(json).has_value());
        }
    });

    runner.run("input is bounded before parsing", [&] {
        std::string maximum(kMaxSettingsJsonBytes, ' ');
        const auto bounded = parse_settings_json(maximum);
        CHECK(!bounded.has_value());
        CHECK_EQ(bounded.error(), SettingsJsonError::MalformedJson);

        maximum.push_back(' ');
        const auto oversized = parse_settings_json(maximum);
        CHECK(!oversized.has_value());
        CHECK_EQ(oversized.error(), SettingsJsonError::TooLarge);
    });

    runner.run("invalid UTF-8 and malformed JSON are rejected", [&] {
        auto invalid_utf8 = std::string(kValidJson);
        invalid_utf8[invalid_utf8.find("Mika")] = static_cast<char>(0xff);
        const auto utf8_result = parse_settings_json(invalid_utf8);
        CHECK(!utf8_result.has_value());
        CHECK_EQ(utf8_result.error(), SettingsJsonError::InvalidUtf8);

        const std::array<std::string, 8U> malformed{
            "",
            "{",
            "[]",
            "{\"format\":}",
            "{\"format\":\"bad\\q\"}",
            "{\"format\":\"\\ud800\"}",
            "{\"format\":\"\\udc00\"}",
            std::string(kValidJson) + "false",
        };
        for (const auto& json : malformed) {
            CHECK(!parse_settings_json(json).has_value());
        }
    });

    runner.run("excessive nesting is rejected with a bounded parser depth", [&] {
        std::string nested(13U, '[');
        nested.append(13U, ']');
        const auto parsed = parse_settings_json(nested);
        CHECK(!parsed.has_value());
        CHECK_EQ(parsed.error(), SettingsJsonError::ExcessiveNesting);
    });

    runner.run("duplicate keys including escaped aliases are rejected", [&] {
        const auto duplicate = replace_once(
            std::string(kValidJson),
            "\"format\":\"lora-messenger.settings\",",
            "\"format\":\"lora-messenger.settings\","
            "\"\\u0066ormat\":\"lora-messenger.settings\",");
        const auto root_result = parse_settings_json(duplicate);
        CHECK(!root_result.has_value());
        CHECK_EQ(root_result.error(), SettingsJsonError::DuplicateKey);

        const auto nested = replace_once(
            std::string(kValidJson), "\"user_id\":\"Mika\",",
            "\"user_id\":\"Mika\",\"user_id\":\"Other\",");
        const auto nested_result = parse_settings_json(nested);
        CHECK(!nested_result.has_value());
        CHECK_EQ(nested_result.error(), SettingsJsonError::DuplicateKey);
    });

    runner.run("unknown and missing fields are rejected at every level", [&] {
        const auto unknown_root = replace_once(
            std::string(kValidJson), "\"schema_version\":1,",
            "\"schema_version\":1,\"future\":true,");
        const auto unknown_identity = replace_once(
            std::string(kValidJson), "\"user_id\":\"Mika\",",
            "\"user_id\":\"Mika\",\"future\":true,");
        const auto unknown_ui = replace_once(
            std::string(kValidJson), "\"locale\":\"en\"",
            "\"locale\":\"en\",\"future\":true");
        const auto unknown_history = replace_once(
            std::string(kValidJson), "\"initialized\":true",
            "\"initialized\":true,\"future\":true");
        for (const auto& json :
             {unknown_root, unknown_identity, unknown_ui, unknown_history}) {
            const auto result = parse_settings_json(json);
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::UnknownKey);
        }

        const auto missing_root = replace_once(
            std::string(kValidJson), "\"generation\":\"1\",", "");
        const auto missing_identity = replace_once(
            std::string(kValidJson), "\"user_id\":\"Mika\",", "");
        const auto missing_ui = replace_once(
            std::string(kValidJson), "\"locale\":\"en\"", "");
        const auto missing_history_root = replace_once(
            std::string(kValidJson),
            ",\"history\":{\"initialized\":true}", "");
        const auto missing_history_field = replace_once(
            std::string(kValidJson), "\"initialized\":true", "");
        for (const auto& json :
             {missing_root, missing_identity, missing_ui,
              missing_history_root, missing_history_field}) {
            const auto result = parse_settings_json(json);
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::MissingKey);
        }
    });

    runner.run("wrong JSON types are rejected rather than coerced", [&] {
        for (const auto& json : {
                 replace_once(std::string(kValidJson),
                              "\"schema_version\":1",
                              "\"schema_version\":\"1\""),
                 replace_once(std::string(kValidJson), "\"generation\":\"1\"",
                              "\"generation\":1"),
                 replace_once(std::string(kValidJson),
                              "\"identity\":{", "\"identity\":["),
                 replace_once(std::string(kValidJson),
                              "\"install_uuid\":"
                              "\"00112233-4455-4677-8899-aabbccddeeff\"",
                              "\"install_uuid\":null"),
                 replace_once(std::string(kValidJson), "\"locale\":\"en\"",
                              "\"locale\":true"),
             }) {
            const auto result = parse_settings_json(json);
            CHECK(!result.has_value());
        }

        for (const auto& json : {
                 replace_once(std::string(kValidJson),
                              "\"history\":{\"initialized\":true}",
                              "\"history\":[]"),
                 replace_once(std::string(kValidJson),
                              "\"initialized\":true",
                              "\"initialized\":\"true\""),
             }) {
            const auto result = parse_settings_json(json);
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::WrongType);
        }
    });

    runner.run("format and schema version are exact", [&] {
        const auto format = parse_settings_json(replace_once(
            std::string(kValidJson), "lora-messenger.settings",
            "lora-messenger.settings.v2"));
        CHECK(!format.has_value());
        CHECK_EQ(format.error(), SettingsJsonError::UnsupportedFormat);

        for (const auto version : {"0", "3", "1.0", "1e0"}) {
            const auto result = parse_settings_json(replace_once(
                std::string(kValidJson), "\"schema_version\":1",
                std::string("\"schema_version\":") + version));
            CHECK(!result.has_value());
            CHECK_EQ(result.error(),
                     SettingsJsonError::UnsupportedSchemaVersion);
        }
    });

    runner.run("uint64 strings reject zero generation noncanonical and overflow", [&] {
        for (const auto generation :
             {"0", "01", "+1", "-1", "18446744073709551616"}) {
            const auto result = parse_settings_json(replace_once(
                std::string(kValidJson), "\"generation\":\"1\"",
                std::string("\"generation\":\"") + generation + "\""));
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::InvalidGeneration);
        }

        for (const auto sequence :
             {"00", "+1", "-1", "1.0", "18446744073709551616"}) {
            const auto result = parse_settings_json(replace_once(
                std::string(kValidJson),
                "\"sender_sequence_high_watermark\":\"0\"",
                std::string("\"sender_sequence_high_watermark\":\"") +
                    sequence + "\""));
            CHECK(!result.has_value());
            CHECK_EQ(result.error(),
                     SettingsJsonError::InvalidSenderSequence);
        }

        const auto maximum = parse_settings_json(replace_once(
            std::string(kValidJson),
            "\"sender_sequence_high_watermark\":\"0\"",
            "\"sender_sequence_high_watermark\":"
            "\"18446744073709551615\""));
        REQUIRE(maximum.has_value());
        CHECK_EQ(maximum.value().sender_sequence_high_watermark,
                 std::numeric_limits<std::uint64_t>::max());
    });

    runner.run("install UUID must be lowercase canonical non-nil v4", [&] {
        for (const auto uuid : {
                 "00112233-4455-4677-8899-AABBCCDDEEFF",
                 "00112233-4455-3677-8899-aabbccddeeff",
                 "00112233-4455-4677-0899-aabbccddeeff",
                 "00000000-0000-0000-0000-000000000000",
                 "00112233445546778899aabbccddeeff",
             }) {
            const auto result = parse_settings_json(replace_once(
                std::string(kValidJson),
                "00112233-4455-4677-8899-aabbccddeeff", uuid));
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::InvalidInstallUuid);
        }
    });

    runner.run("user ID and locale retain domain validation", [&] {
        for (const auto user : {"", " Mika", "Mika ", "Mika\\nOther",
                                "1234567890123456789012345"}) {
            const auto result = parse_settings_json(replace_once(
                std::string(kValidJson), "\"user_id\":\"Mika\"",
                std::string("\"user_id\":\"") + user + "\""));
            CHECK(!result.has_value());
            CHECK_EQ(result.error(), SettingsJsonError::InvalidUserId);
        }

        const auto locale = parse_settings_json(replace_once(
            std::string(kValidJson), "\"locale\":\"en\"",
            "\"locale\":\"EN\""));
        CHECK(!locale.has_value());
        CHECK_EQ(locale.error(), SettingsJsonError::InvalidLocale);
    });

    runner.run("serializer rejects impossible generation and locale values", [&] {
        const auto generation = serialize_settings_json(make_record(0U));
        CHECK(!generation.has_value());
        CHECK_EQ(generation.error(), SettingsJsonError::InvalidGeneration);

        const auto locale = serialize_settings_json(make_record(
            1U, 0U, static_cast<StoredLocale>(99)));
        CHECK(!locale.has_value());
        CHECK_EQ(locale.error(), SettingsJsonError::InvalidLocale);
    });

    return runner.finish();
}
