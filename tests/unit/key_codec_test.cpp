/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/key_codec.h"

#include "test_support.h"

#include <cstdint>
#include <initializer_list>

namespace {

// Mirrors how lv_sdl_keyboard.c's sdl_keyboard_read() actually fills
// data->key: `lv_memcpy(&data->key, dev->buf, utf8_len)` on a little-endian
// target, i.e. the first UTF-8 byte lands in the least-significant byte.
// Unlisted trailing bytes stay zero, matching data->key's `= 0` initializer.
std::uint32_t pack(std::initializer_list<unsigned int> utf8_bytes) {
    std::uint32_t key = 0;
    int shift = 0;
    for (const auto byte : utf8_bytes) {
        key |= (byte & 0xffU) << shift;
        shift += 8;
    }
    return key;
}

} // namespace

int main() {
    using platform::decode_packed_utf8_key;
    lora::test::Runner runner;

    runner.run("single-byte ASCII round-trips through the packed encoding", [&] {
        CHECK_EQ(decode_packed_utf8_key(pack({'A'})), U'A');
        CHECK_EQ(decode_packed_utf8_key(pack({'0'})), U'0');
        CHECK_EQ(decode_packed_utf8_key(pack({' '})), U' ');
    });

    runner.run("two-byte UTF-8 decodes to its codepoint (U+00A2 CENT SIGN)", [&] {
        CHECK_EQ(decode_packed_utf8_key(pack({0xc2, 0xa2})), static_cast<char32_t>(0x00a2));
    });

    runner.run("three-byte UTF-8 decodes Japanese/Chinese codepoints correctly", [&] {
        // U+3042 HIRAGANA LETTER A ("あ"): E3 81 82.
        CHECK_EQ(decode_packed_utf8_key(pack({0xe3, 0x81, 0x82})), static_cast<char32_t>(0x3042));
        // U+4E2D CJK UNIFIED IDEOGRAPH ("中"): E4 B8 AD.
        CHECK_EQ(decode_packed_utf8_key(pack({0xe4, 0xb8, 0xad})), static_cast<char32_t>(0x4e2d));
    });

    runner.run("four-byte UTF-8 decodes supplementary-plane codepoints", [&] {
        // U+1F680 ROCKET ("🚀"): F0 9F 9A 80.
        CHECK_EQ(decode_packed_utf8_key(pack({0xf0, 0x9f, 0x9a, 0x80})), static_cast<char32_t>(0x1f680));
    });

    runner.run("malformed packed input falls back to the unsupported-input marker", [&] {
        // Lone continuation byte as the lead byte.
        CHECK_EQ(decode_packed_utf8_key(pack({0x80})), static_cast<char32_t>(0x25a1));
        // Valid three-byte lead, but the second byte is not a continuation byte.
        CHECK_EQ(decode_packed_utf8_key(pack({0xe3, 0x20, 0x82})), static_cast<char32_t>(0x25a1));
        // Reserved lead byte (0xff is never valid UTF-8).
        CHECK_EQ(decode_packed_utf8_key(pack({0xff})), static_cast<char32_t>(0x25a1));
    });

    return runner.finish();
}
