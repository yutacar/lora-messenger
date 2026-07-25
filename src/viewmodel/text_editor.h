/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace lora::viewmodel {

enum class EditorError {
    None,
    InvalidUtf8,
    InvalidCodepoint,
    TooLong,
};

class TextEditor {
public:
    explicit TextEditor(std::size_t maximum_bytes);

    EditorError set_text(std::string_view text);
    EditorError insert(char32_t codepoint);
    EditorError insert_utf8(std::string_view text);

    bool move_left() noexcept;
    bool move_right() noexcept;
    bool backspace() noexcept;
    bool delete_forward() noexcept;
    void clear() noexcept;

    const std::string& text() const noexcept;
    std::size_t cursor_byte_offset() const noexcept;
    std::size_t maximum_bytes() const noexcept;
    std::size_t remaining_bytes() const noexcept;
    bool empty() const noexcept;

private:
    std::size_t previous_boundary(std::size_t offset) const noexcept;
    std::size_t next_boundary(std::size_t offset) const noexcept;

    std::size_t maximum_bytes_;
    std::string text_;
    std::size_t cursor_{0};
};

} // namespace lora::viewmodel
