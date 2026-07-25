/*
 * SPDX-License-Identifier: MIT
 */

#include "core/text.h"

#include "test_support.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace {

std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

struct EncodingCase {
    const char* name;
    std::string value;
};

} // namespace

int main() {
    using lora::core::PostBody;
    using lora::core::TextError;
    using lora::core::UserId;
    using lora::core::is_valid_utf8;
    lora::test::Runner runner;

    runner.run("UTF-8 validator accepts scalar values across encodings", [&] {
        CHECK(is_valid_utf8("plain ASCII"));
        CHECK(is_valid_utf8(u8"日本語"));
        CHECK(is_valid_utf8(u8"📻"));
        CHECK(is_valid_utf8(u8"A¢あ🚀"));
    });

    const std::vector<EncodingCase> valid_boundaries{
        {"U+0000", bytes({0x00})},
        {"U+007F", bytes({0x7f})},
        {"U+0080", bytes({0xc2, 0x80})},
        {"U+07FF", bytes({0xdf, 0xbf})},
        {"U+0800", bytes({0xe0, 0xa0, 0x80})},
        {"U+D7FF", bytes({0xed, 0x9f, 0xbf})},
        {"U+E000", bytes({0xee, 0x80, 0x80})},
        {"U+10000", bytes({0xf0, 0x90, 0x80, 0x80})},
        {"U+10FFFF", bytes({0xf4, 0x8f, 0xbf, 0xbf})},
    };
    for (const auto& sample : valid_boundaries) {
        runner.run(std::string("UTF-8 scalar boundary accepts ") + sample.name, [&] {
            CHECK(is_valid_utf8(sample.value));
        });
    }

    const std::vector<EncodingCase> invalid_encodings{
        {"lone continuation 80", bytes({0x80})},
        {"lone continuation BF", bytes({0xbf})},
        {"invalid lead C0", bytes({0xc0, 0x80})},
        {"invalid lead C1", bytes({0xc1, 0xbf})},
        {"invalid lead F5", bytes({0xf5, 0x80, 0x80, 0x80})},
        {"invalid lead FE", bytes({0xfe})},
        {"invalid lead FF", bytes({0xff})},
        {"truncated two-byte", bytes({0xc2})},
        {"truncated three-byte one", bytes({0xe0})},
        {"truncated three-byte two", bytes({0xe0, 0xa0})},
        {"truncated four-byte one", bytes({0xf0})},
        {"truncated four-byte two", bytes({0xf0, 0x90})},
        {"truncated four-byte three", bytes({0xf0, 0x90, 0x80})},
        {"two-byte bad continuation", bytes({0xc2, 0x20})},
        {"three-byte bad first continuation", bytes({0xe1, 0x20, 0x80})},
        {"three-byte bad second continuation", bytes({0xe1, 0x80, 0x20})},
        {"four-byte bad first continuation", bytes({0xf1, 0x20, 0x80, 0x80})},
        {"four-byte bad second continuation", bytes({0xf1, 0x80, 0x20, 0x80})},
        {"four-byte bad third continuation", bytes({0xf1, 0x80, 0x80, 0x20})},
        {"overlong two-byte", bytes({0xc0, 0xaf})},
        {"overlong three-byte", bytes({0xe0, 0x80, 0x80})},
        {"overlong four-byte", bytes({0xf0, 0x80, 0x80, 0x80})},
        {"surrogate lower bound", bytes({0xed, 0xa0, 0x80})},
        {"surrogate upper bound", bytes({0xed, 0xbf, 0xbf})},
        {"above U+10FFFF", bytes({0xf4, 0x90, 0x80, 0x80})},
    };
    for (const auto& sample : invalid_encodings) {
        runner.run(std::string("UTF-8 rejects ") + sample.name, [&] {
            CHECK(!is_valid_utf8(sample.value));

            std::string embedded = "a";
            embedded += sample.value;
            embedded += 'b';
            const auto user = UserId::create(embedded);
            CHECK(!user.has_value());
            CHECK_EQ(user.error(), TextError::InvalidUtf8);
            const auto body = PostBody::create(embedded);
            CHECK(!body.has_value());
            CHECK_EQ(body.error(), TextError::InvalidUtf8);
        });
    }

    runner.run("user IDs enforce byte limits including multibyte text", [&] {
        const auto minimum = UserId::create("a");
        REQUIRE(minimum.has_value());
        CHECK_EQ(minimum.value().value().size(), 1U);

        const auto below_maximum = UserId::create(std::string(23, 'a'));
        REQUIRE(below_maximum.has_value());
        CHECK_EQ(below_maximum.value().value().size(), 23U);

        const auto exact_ascii = UserId::create(std::string(24, 'a'));
        REQUIRE(exact_ascii.has_value());
        CHECK_EQ(exact_ascii.value().value().size(), 24U);

        const auto over_ascii = UserId::create(std::string(25, 'a'));
        CHECK(!over_ascii.has_value());
        CHECK_EQ(over_ascii.error(), TextError::TooLong);

        const auto exact_multibyte = UserId::create(u8"界界界界界界界界");
        REQUIRE(exact_multibyte.has_value());
        CHECK_EQ(exact_multibyte.value().value().size(), 24U);

        const auto over_multibyte = UserId::create(u8"界界界界界界界界界");
        CHECK(!over_multibyte.has_value());
        CHECK_EQ(over_multibyte.error(), TextError::TooLong);
    });

    runner.run("post bodies enforce their 160-byte limit", [&] {
        const auto minimum = PostBody::create("b");
        REQUIRE(minimum.has_value());
        CHECK_EQ(minimum.value().value().size(), 1U);

        const auto below_maximum = PostBody::create(std::string(159, 'b'));
        REQUIRE(below_maximum.has_value());
        CHECK_EQ(below_maximum.value().value().size(), 159U);

        const auto exact = PostBody::create(std::string(160, 'b'));
        REQUIRE(exact.has_value());
        CHECK_EQ(exact.value().value().size(), 160U);

        const auto over = PostBody::create(std::string(161, 'b'));
        CHECK(!over.has_value());
        CHECK_EQ(over.error(), TextError::TooLong);

        std::string multibyte_159;
        for (int index = 0; index < 53; ++index) {
            multibyte_159 += u8"界";
        }
        const auto multibyte_below = PostBody::create(multibyte_159);
        REQUIRE(multibyte_below.has_value());
        CHECK_EQ(multibyte_below.value().value().size(), 159U);

        const auto multibyte_over = PostBody::create(multibyte_below.value().value() + u8"界");
        CHECK(!multibyte_over.has_value());
        CHECK_EQ(multibyte_over.error(), TextError::TooLong);
    });

    runner.run("empty and whitespace-only values are rejected", [&] {
        const auto empty_user = UserId::create("");
        CHECK(!empty_user.has_value());
        CHECK_EQ(empty_user.error(), TextError::Empty);

        const auto space_user = UserId::create("   ");
        CHECK(!space_user.has_value());
        CHECK_EQ(space_user.error(), TextError::Empty);

        const auto unicode_space_body = PostBody::create(u8"\u00a0\u3000");
        CHECK(!unicode_space_body.has_value());
        CHECK_EQ(unicode_space_body.error(), TextError::Empty);

        const auto only_lines = PostBody::create("\n\n");
        CHECK(!only_lines.has_value());
        CHECK_EQ(only_lines.error(), TextError::Empty);
    });

    runner.run("user IDs reject edge whitespace and line breaks", [&] {
        const auto leading = UserId::create(" alice");
        CHECK(!leading.has_value());
        CHECK_EQ(leading.error(), TextError::EdgeWhitespace);

        const auto trailing = UserId::create(u8"alice\u3000");
        CHECK(!trailing.has_value());
        CHECK_EQ(trailing.error(), TextError::EdgeWhitespace);

        const auto newline = UserId::create("alice\nbob");
        CHECK(!newline.has_value());
        CHECK_EQ(newline.error(), TextError::ForbiddenCharacter);
    });

    runner.run("post bodies allow LF and edge spaces but no other controls", [&] {
        const auto multiline = PostBody::create(" first\nsecond ");
        REQUIRE(multiline.has_value());
        CHECK_EQ(multiline.value().value(), " first\nsecond ");

        const auto tab = PostBody::create("a\tb");
        CHECK(!tab.has_value());
        CHECK_EQ(tab.error(), TextError::ForbiddenCharacter);

        const auto carriage_return = PostBody::create("a\rb");
        CHECK(!carriage_return.has_value());
        CHECK_EQ(carriage_return.error(), TextError::ForbiddenCharacter);

        const auto line_separator = PostBody::create(u8"a\u2028b");
        CHECK(!line_separator.has_value());
        CHECK_EQ(line_separator.error(), TextError::ForbiddenCharacter);

        const auto paragraph_separator = PostBody::create(u8"a\u2029b");
        CHECK(!paragraph_separator.has_value());
        CHECK_EQ(paragraph_separator.error(), TextError::ForbiddenCharacter);
    });

    runner.run("embedded NUL, C1, bidi controls, and noncharacters are forbidden", [&] {
        const auto nul = PostBody::create(std::string("a\0b", 3));
        CHECK(!nul.has_value());
        CHECK_EQ(nul.error(), TextError::ForbiddenCharacter);

        const auto c1 = PostBody::create(std::string("a\xc2\x85" "b", 4));
        CHECK(!c1.has_value());
        CHECK_EQ(c1.error(), TextError::ForbiddenCharacter);

        const auto bidi = UserId::create(u8"ab\u202ecd");
        CHECK(!bidi.has_value());
        CHECK_EQ(bidi.error(), TextError::ForbiddenCharacter);

        const auto noncharacter = PostBody::create(std::string("a\xef\xbf\xbf" "b", 5));
        CHECK(!noncharacter.has_value());
        CHECK_EQ(noncharacter.error(), TextError::ForbiddenCharacter);
    });

    runner.run("invalid UTF-8 is reported by value factories", [&] {
        const auto user = UserId::create(std::string("a\xe2\x82", 3));
        CHECK(!user.has_value());
        CHECK_EQ(user.error(), TextError::InvalidUtf8);

        const auto body = PostBody::create(std::string("\xed\xa0\x80", 3));
        CHECK(!body.has_value());
        CHECK_EQ(body.error(), TextError::InvalidUtf8);
    });

    runner.run("accepted text is preserved without normalization", [&] {
        const auto composed = UserId::create(u8"café");
        const auto decomposed = UserId::create(u8"café");
        REQUIRE(composed.has_value());
        REQUIRE(decomposed.has_value());
        CHECK_NE(composed.value().value(), decomposed.value().value());
        CHECK_EQ(composed.value().value(), std::string(u8"café"));
        CHECK_EQ(decomposed.value().value(), std::string(u8"café"));
    });

    return runner.finish();
}
