/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace platform {

// LVGL's SDL keyboard indev (lv_sdl_keyboard.c: sdl_keyboard_read()) packs an
// entire UTF-8-encoded character -- not a Unicode codepoint -- into an
// indev's `key` value, least-significant byte first, whenever the character
// came from an SDL_TEXTINPUT event. That is exactly how the host OS's native
// IME delivers composed text (Japanese, Chinese, any non-ASCII input), so a
// 3-byte CJK character arrives as a 3-byte-packed uint32_t, not as its
// codepoint. LVGL's own lv_textarea widget knows to unpack this
// (lv_textarea_add_char() rebuilds a little-endian byte buffer from `c` and
// re-decodes it with lv_text_encoded_next()); code that reads
// lv_indev_get_key() directly instead of going through lv_textarea has to do
// the same unpacking itself, or every multi-byte character silently turns
// into garbage.
//
// ASCII (single-byte UTF-8) round-trips unchanged: byte 0's high bit is 0,
// so the decoded length is 1 and the codepoint is just that byte, same as a
// plain cast. Malformed packed input (should not occur from a real LVGL
// indev, but this function must still be total) returns U+25A1 ('□'), this
// app's existing unsupported-input marker (see
// src/view/screens/messenger_screen.cpp's own decode_scalar()).
inline char32_t decode_packed_utf8_key(std::uint32_t key) noexcept {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(key & 0xffU),
        static_cast<unsigned char>((key >> 8U) & 0xffU),
        static_cast<unsigned char>((key >> 16U) & 0xffU),
        static_cast<unsigned char>((key >> 24U) & 0xffU),
    };
    const unsigned char lead = bytes[0];
    std::size_t length = 1;
    std::uint32_t codepoint = lead;
    if ((lead & 0x80U) == 0x00U) {
        length = 1;
        codepoint = lead;
    } else if ((lead & 0xe0U) == 0xc0U) {
        length = 2;
        codepoint = lead & 0x1fU;
    } else if ((lead & 0xf0U) == 0xe0U) {
        length = 3;
        codepoint = lead & 0x0fU;
    } else if ((lead & 0xf8U) == 0xf0U) {
        length = 4;
        codepoint = lead & 0x07U;
    } else {
        return 0x25a1U;
    }
    for (std::size_t index = 1; index < length; ++index) {
        if ((bytes[index] & 0xc0U) != 0x80U) {
            return 0x25a1U;
        }
        codepoint = (codepoint << 6U) | (bytes[index] & 0x3fU);
    }
    return static_cast<char32_t>(codepoint);
}

} // namespace platform
