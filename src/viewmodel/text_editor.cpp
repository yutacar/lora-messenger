/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/text_editor.h"

#include "core/text.h"

#include <array>
#include <cstdint>

namespace lora::viewmodel {
namespace {

bool is_continuation(unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

std::size_t encode_utf8(char32_t value, std::array<char, 4>& output) noexcept {
    const auto codepoint = static_cast<std::uint32_t>(value);
    if (codepoint <= 0x7fU) {
        output[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint <= 0x7ffU) {
        output[0] = static_cast<char>(0xc0U | (codepoint >> 6U));
        output[1] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 2;
    }
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
        return 0;
    }
    if (codepoint <= 0xffffU) {
        output[0] = static_cast<char>(0xe0U | (codepoint >> 12U));
        output[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
        output[2] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 3;
    }
    if (codepoint <= 0x10ffffU) {
        output[0] = static_cast<char>(0xf0U | (codepoint >> 18U));
        output[1] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU));
        output[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU));
        output[3] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 4;
    }
    return 0;
}

} // namespace

TextEditor::TextEditor(std::size_t maximum_bytes)
    : maximum_bytes_(maximum_bytes) {}

EditorError TextEditor::set_text(std::string_view text) {
    if (!core::is_valid_utf8(text)) {
        return EditorError::InvalidUtf8;
    }
    if (text.size() > maximum_bytes_) {
        return EditorError::TooLong;
    }
    text_ = std::string(text);
    cursor_ = text_.size();
    return EditorError::None;
}

EditorError TextEditor::insert(char32_t codepoint) {
    std::array<char, 4> encoded{};
    const auto size = encode_utf8(codepoint, encoded);
    if (size == 0) {
        return EditorError::InvalidCodepoint;
    }
    if (text_.size() + size > maximum_bytes_) {
        return EditorError::TooLong;
    }
    text_.insert(cursor_, encoded.data(), size);
    cursor_ += size;
    return EditorError::None;
}

EditorError TextEditor::insert_utf8(std::string_view text) {
    if (!core::is_valid_utf8(text)) {
        return EditorError::InvalidUtf8;
    }
    if (text_.size() + text.size() > maximum_bytes_) {
        return EditorError::TooLong;
    }
    const std::string insertion(text);
    text_.insert(cursor_, insertion);
    cursor_ += text.size();
    return EditorError::None;
}

bool TextEditor::move_left() noexcept {
    if (cursor_ == 0) {
        return false;
    }
    cursor_ = previous_boundary(cursor_);
    return true;
}

bool TextEditor::move_right() noexcept {
    if (cursor_ >= text_.size()) {
        return false;
    }
    cursor_ = next_boundary(cursor_);
    return true;
}

bool TextEditor::backspace() noexcept {
    if (cursor_ == 0) {
        return false;
    }
    const auto previous = previous_boundary(cursor_);
    text_.erase(previous, cursor_ - previous);
    cursor_ = previous;
    return true;
}

bool TextEditor::delete_forward() noexcept {
    if (cursor_ >= text_.size()) {
        return false;
    }
    const auto next = next_boundary(cursor_);
    text_.erase(cursor_, next - cursor_);
    return true;
}

void TextEditor::clear() noexcept {
    text_.clear();
    cursor_ = 0;
}

const std::string& TextEditor::text() const noexcept { return text_; }
std::size_t TextEditor::cursor_byte_offset() const noexcept { return cursor_; }
std::size_t TextEditor::maximum_bytes() const noexcept { return maximum_bytes_; }
std::size_t TextEditor::remaining_bytes() const noexcept {
    return maximum_bytes_ - text_.size();
}
bool TextEditor::empty() const noexcept { return text_.empty(); }

std::size_t TextEditor::previous_boundary(std::size_t offset) const noexcept {
    --offset;
    while (offset > 0 && is_continuation(static_cast<unsigned char>(text_[offset]))) {
        --offset;
    }
    return offset;
}

std::size_t TextEditor::next_boundary(std::size_t offset) const noexcept {
    ++offset;
    while (offset < text_.size() &&
           is_continuation(static_cast<unsigned char>(text_[offset]))) {
        ++offset;
    }
    return offset;
}

} // namespace lora::viewmodel
