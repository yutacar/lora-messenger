/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/text_editor.h"

#include "core/text.h"
#include "test_support.h"

#include <string>

int main() {
    using lora::viewmodel::EditorError;
    using lora::viewmodel::TextEditor;
    lora::test::Runner runner;

    runner.run("empty intermediate text and edge cursor operations are safe", [&] {
        TextEditor editor(160);
        CHECK(editor.empty());
        CHECK_EQ(editor.cursor_byte_offset(), 0U);
        CHECK_EQ(editor.remaining_bytes(), 160U);
        CHECK(!editor.move_left());
        CHECK(!editor.move_right());
        CHECK(!editor.backspace());
        CHECK(!editor.delete_forward());
        CHECK_EQ(editor.set_text(""), EditorError::None);
        CHECK(editor.empty());
    });

    runner.run("ASCII insertion enforces the exact byte boundary atomically", [&] {
        TextEditor editor(160);
        for (std::size_t index = 0; index < 160; ++index) {
            CHECK_EQ(editor.insert(U'x'), EditorError::None);
        }
        CHECK_EQ(editor.text().size(), 160U);
        CHECK_EQ(editor.cursor_byte_offset(), 160U);
        CHECK_EQ(editor.remaining_bytes(), 0U);
        const auto before = editor.text();
        CHECK_EQ(editor.insert(U'y'), EditorError::TooLong);
        CHECK_EQ(editor.text(), before);
        CHECK_EQ(editor.cursor_byte_offset(), 160U);
    });

    runner.run("movement deletion and insertion preserve codepoint boundaries", [&] {
        TextEditor editor(32);
        REQUIRE(editor.set_text("A日😀B") == EditorError::None);
        CHECK_EQ(editor.text().size(), 9U);
        CHECK(editor.move_left());
        CHECK_EQ(editor.cursor_byte_offset(), 8U);
        CHECK(editor.move_left());
        CHECK_EQ(editor.cursor_byte_offset(), 4U);
        CHECK_EQ(editor.insert(U'中'), EditorError::None);
        CHECK_EQ(editor.text(), "A日中😀B");
        CHECK_EQ(editor.cursor_byte_offset(), 7U);
        CHECK(editor.backspace());
        CHECK_EQ(editor.text(), "A日😀B");
        CHECK_EQ(editor.cursor_byte_offset(), 4U);
        CHECK(editor.delete_forward());
        CHECK_EQ(editor.text(), "A日B");
        CHECK_EQ(editor.cursor_byte_offset(), 4U);
        CHECK(lora::core::is_valid_utf8(editor.text()));
    });

    runner.run("bulk UTF-8 insertion is all-or-nothing at the byte limit", [&] {
        TextEditor editor(10);
        CHECK_EQ(editor.insert_utf8("日本"), EditorError::None);
        CHECK_EQ(editor.text().size(), 6U);
        const auto before = editor.text();
        CHECK_EQ(editor.insert_utf8("中文"), EditorError::TooLong);
        CHECK_EQ(editor.text(), before);
        CHECK_EQ(editor.cursor_byte_offset(), 6U);
        CHECK_EQ(editor.insert_utf8("😀"), EditorError::None);
        CHECK_EQ(editor.text(), "日本😀");
        CHECK_EQ(editor.remaining_bytes(), 0U);
    });

    runner.run("malformed UTF-8 and invalid scalars preserve prior state", [&] {
        TextEditor editor(32);
        REQUIRE(editor.set_text("safe") == EditorError::None);
        const std::string malformed{"\xE2\x82", 2};
        CHECK_EQ(editor.set_text(malformed), EditorError::InvalidUtf8);
        CHECK_EQ(editor.text(), "safe");
        CHECK_EQ(editor.insert_utf8(malformed), EditorError::InvalidUtf8);
        CHECK_EQ(editor.text(), "safe");
        CHECK_EQ(editor.insert(static_cast<char32_t>(0xd800U)),
                 EditorError::InvalidCodepoint);
        CHECK_EQ(editor.insert(static_cast<char32_t>(0x110000U)),
                 EditorError::InvalidCodepoint);
        CHECK_EQ(editor.text(), "safe");
        CHECK_EQ(editor.cursor_byte_offset(), 4U);
    });

    runner.run("self-referential bulk edits do not invalidate their source", [&] {
        TextEditor editor(16);
        REQUIRE(editor.set_text("日") == EditorError::None);
        CHECK_EQ(editor.insert_utf8(editor.text()), EditorError::None);
        CHECK_EQ(editor.text(), "日日");
        CHECK_EQ(editor.set_text(editor.text()), EditorError::None);
        CHECK_EQ(editor.text(), "日日");
        CHECK_EQ(editor.cursor_byte_offset(), 6U);
    });

    runner.run("mixed multibyte content can occupy exactly 160 bytes", [&] {
        TextEditor editor(160);
        std::string exact;
        for (std::size_t index = 0; index < 52; ++index) {
            exact += "日";
        }
        exact += "test";
        REQUIRE(exact.size() == 160U);
        CHECK_EQ(editor.set_text(exact), EditorError::None);
        CHECK_EQ(editor.remaining_bytes(), 0U);
        CHECK_EQ(editor.insert(U'語'), EditorError::TooLong);
        CHECK_EQ(editor.text(), exact);
        editor.clear();
        CHECK(editor.empty());
        CHECK_EQ(editor.remaining_bytes(), 160U);
    });

    return runner.finish();
}
