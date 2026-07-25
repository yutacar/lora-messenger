/*
 * SPDX-License-Identifier: MIT
 */

#include "core/uuid.h"

#include "test_fakes.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

int main() {
    using lora::core::IdError;
    using lora::core::InstallId;
    using lora::core::MessageId;
    using lora::core::Uuid;
    lora::test::Runner runner;

    runner.run("canonical UUID text round trips and normalizes hex case", [&] {
        const auto parsed = Uuid::parse("00112233-4455-4677-8899-aabbccddeeff");
        REQUIRE(parsed.has_value());
        CHECK_EQ(parsed->to_string(), "00112233-4455-4677-8899-aabbccddeeff");
        CHECK(parsed->is_v4());

        const auto uppercase = Uuid::parse("00112233-4455-4677-8899-AABBCCDDEEFF");
        REQUIRE(uppercase.has_value());
        CHECK_EQ(*uppercase, *parsed);
        CHECK_EQ(uppercase->to_string(), parsed->to_string());
    });

    runner.run("UUID parser rejects malformed representations", [&] {
        CHECK(!Uuid::parse("00112233445546778899aabbccddeeff").has_value());
        CHECK(!Uuid::parse("00112233_4455-4677-8899-aabbccddeeff").has_value());
        CHECK(!Uuid::parse("00112233-4455-4677-8899-aabbccddeezz").has_value());
        CHECK(!Uuid::parse("00112233-4455-4677-8899-aabbccddeef").has_value());
        CHECK(!Uuid::parse("00112233-4455-4677-8899-aabbccddeeff0").has_value());
    });

    runner.run("v4 validation requires a non-nil RFC variant UUID", [&] {
        Uuid::Bytes nil_bytes{};
        const auto nil = Uuid::from_bytes(nil_bytes);
        CHECK(nil.is_nil());
        CHECK(!nil.is_v4());

        auto wrong_version = lora::test::make_uuid(1).bytes();
        wrong_version[6] = static_cast<std::uint8_t>((wrong_version[6] & 0x0fU) | 0x30U);
        CHECK(!Uuid::from_bytes(wrong_version).is_v4());

        auto wrong_variant = lora::test::make_uuid(2).bytes();
        wrong_variant[8] = static_cast<std::uint8_t>(wrong_variant[8] & 0x3fU);
        CHECK(!Uuid::from_bytes(wrong_variant).is_v4());
    });

    runner.run("generation requests 16 bytes once and forces v4 bits", [&] {
        lora::test::ScriptedRandom random;
        random.push_bytes(std::vector<std::uint8_t>(16, 0xffU));
        const auto generated = Uuid::generate_v4(random);
        REQUIRE(generated.has_value());
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(random.last_requested_size(), 16U);
        CHECK(generated->is_v4());
        CHECK_EQ(generated->bytes()[6], 0x4fU);
        CHECK_EQ(generated->bytes()[8], 0xbfU);
    });

    runner.run("random failure is returned without retry", [&] {
        lora::test::ScriptedRandom random;
        random.fail_next();
        const auto install = InstallId::generate(random);
        CHECK(!install.has_value());
        CHECK_EQ(install.error(), IdError::RandomUnavailable);
        CHECK_EQ(random.call_count(), 1U);

        random.fail_next();
        const auto message = MessageId::generate(random);
        CHECK(!message.has_value());
        CHECK_EQ(message.error(), IdError::RandomUnavailable);
        CHECK_EQ(random.call_count(), 2U);
    });

    runner.run("strong identifiers reject nil and non-v4 UUIDs", [&] {
        Uuid::Bytes nil_bytes{};
        const auto nil = Uuid::from_bytes(nil_bytes);
        const auto install = InstallId::from_uuid(nil);
        const auto message = MessageId::from_uuid(nil);
        CHECK(!install.has_value());
        CHECK_EQ(install.error(), IdError::InvalidUuidV4);
        CHECK(!message.has_value());
        CHECK_EQ(message.error(), IdError::InvalidUuidV4);

        auto version_one = lora::test::make_uuid(3).bytes();
        version_one[6] = static_cast<std::uint8_t>((version_one[6] & 0x0fU) | 0x10U);
        CHECK(!InstallId::from_uuid(Uuid::from_bytes(version_one)).has_value());
        CHECK(!MessageId::from_uuid(Uuid::from_bytes(version_one)).has_value());
    });

    runner.run("equal UUIDs hash to the same key", [&] {
        const auto first = lora::test::make_uuid(7);
        const auto same = Uuid::from_bytes(first.bytes());
        const auto other = lora::test::make_uuid(8);
        std::unordered_set<Uuid, lora::core::UuidHash> values;
        values.insert(first);
        values.insert(same);
        values.insert(other);
        CHECK_EQ(values.size(), 2U);
        CHECK(values.find(first) != values.end());
        CHECK(values.find(other) != values.end());
    });

    return runner.finish();
}
