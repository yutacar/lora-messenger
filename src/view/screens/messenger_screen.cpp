/*
 * SPDX-License-Identifier: MIT
 */

#include "messenger_screen.h"

#include "theme.h"
#include "viewmodel/i18n.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace view {
namespace {

using lora::viewmodel::DeliveryBadge;
using lora::viewmodel::Locale;
using lora::viewmodel::ModalId;
using lora::viewmodel::StringId;
using lora::viewmodel::translate;

constexpr int kHeaderHeight = 24;
constexpr int kFooterY = 142;
constexpr int kFooterHeight = 28;

const lv_font_t* usable_font(const lv_font_t* font, const lv_font_t* fallback) {
    return font ? font : fallback;
}

bool decode_scalar(std::string_view text, std::size_t& offset,
                   std::uint32_t& scalar) noexcept {
    if (offset >= text.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (first <= 0x7FU) {
        length = 1;
        value = first;
    } else if (first >= 0xC2U && first <= 0xDFU) {
        length = 2;
        value = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3;
        value = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4;
        value = first & 0x07U;
    } else {
        ++offset;
        scalar = 0x25A1U;
        return true;
    }
    if (offset + length > text.size()) {
        offset = text.size();
        scalar = 0x25A1U;
        return true;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0U) != 0x80U) {
            ++offset;
            scalar = 0x25A1U;
            return true;
        }
        value = (value << 6U) | (next & 0x3FU);
    }
    offset += length;
    scalar = value;
    return true;
}

void append_scalar(std::string& output, std::uint32_t scalar) {
    if (scalar <= 0x7FU) {
        output.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (scalar >> 6U)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else if (scalar <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (scalar >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (scalar >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
    }
}

bool font_has(const lv_font_t* font, std::uint32_t scalar) noexcept {
    if (!font || scalar == '\n') {
        return true;
    }
    lv_font_glyph_dsc_t descriptor{};
    return lv_font_get_glyph_dsc(font, &descriptor, scalar, 0);
}

std::string display_text(std::string_view text, const lv_font_t* font) {
    std::string result;
    result.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t scalar = 0;
        if (!decode_scalar(text, offset, scalar)) {
            break;
        }
        append_scalar(result, font_has(font, scalar) ? scalar : 0x25A1U);
    }
    return result;
}

std::uint32_t utf8_character_count(std::string_view text) noexcept {
    std::uint32_t count = 0;
    for (const auto byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

lv_obj_t* label(lv_obj_t* parent, std::string_view text,
                const lv_font_t* font, lv_color_t color) {
    auto* object = lv_label_create(parent);
    const auto safe = display_text(text, font);
    lv_label_set_text(object, safe.c_str());
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color, 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    return object;
}

lv_obj_t* panel(lv_obj_t* parent, int x, int y, int width, int height,
                lv_color_t background, lv_color_t border, int radius = 5) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, background, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, border, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

StringId badge_string_id(DeliveryBadge badge) noexcept {
    switch (badge) {
        case DeliveryBadge::Received: return StringId::BadgeReceived;
        case DeliveryBadge::Queued: return StringId::BadgeQueued;
        case DeliveryBadge::Broadcast: return StringId::BadgeBroadcast;
        case DeliveryBadge::Failed: return StringId::BadgeFailed;
        case DeliveryBadge::Unknown: return StringId::BadgeUnknown;
    }
    return StringId::BadgeUnknown;
}

std::string sender_line(const lora::viewmodel::TimelineRowSnapshot& row,
                        Locale locale) {
    std::string line = row.selected ? "> " : "  ";
    line += row.sender;
    if (row.show_sender_suffix) {
        line += " #";
        line += row.sender_suffix;
    }
    line += "  [";
    line += translate(locale, badge_string_id(row.delivery));
    line += "]";
    if (row.mentions_me) {
        line += "  @";
    }
    return line;
}

std::string detail_sender_line(const lora::viewmodel::DetailSnapshot& detail,
                               Locale locale) {
    std::string line = detail.sender;
    if (!detail.sender_suffix.empty()) {
        line += " #";
        line += detail.sender_suffix;
    }
    line += "  [";
    line += translate(locale, badge_string_id(detail.delivery));
    line += "]";
    return line;
}

void render_header(lv_obj_t* root, const lora::viewmodel::ViewSnapshot& snapshot,
                   const LocaleFonts& fonts, const Palette& colors) {
    auto* header = panel(root, 0, 0, kScreenWidth, kHeaderHeight,
                         colors.footer, colors.footer, 0);
    auto* title = label(header, translate(snapshot.locale, StringId::AppTitle),
                        fonts.body, colors.text);
    lv_obj_set_pos(title, 7, 2);
    lv_obj_set_width(title, 178);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    auto* badge = panel(header, 202, 2, 111, 19, lv_color_hex(0x183B3A),
                        colors.accent, 9);
    const auto header_status =
        snapshot.transport_status ==
                lora::viewmodel::TransportStatus::WifiLan
            ? StringId::HeaderLanExit
            : (snapshot.radio_ready
                   ? StringId::HeaderRadioExit
                   : StringId::HeaderDemoExit);
    auto* badge_text = label(badge,
                             translate(snapshot.locale, header_status),
                             fonts.small, colors.accent);
    lv_obj_center(badge_text);
}

void render_footer(lv_obj_t* root, const lora::viewmodel::ViewSnapshot& snapshot,
                   const LocaleFonts& fonts, const Palette& colors) {
    auto* footer = panel(root, 0, kFooterY, kScreenWidth, kFooterHeight,
                         colors.footer, colors.panel_border, 0);
    auto* guide = label(footer, snapshot.footer, fonts.small, colors.text);
    lv_obj_set_width(guide, 306);
    lv_label_set_long_mode(guide, LV_LABEL_LONG_DOT);
    lv_obj_center(guide);
}

void render_page_heading(lv_obj_t* root,
                         const lora::viewmodel::ViewSnapshot& snapshot,
                         const LocaleFonts& fonts, const Palette& colors) {
    auto* heading = label(root, snapshot.title, fonts.body, colors.text);
    lv_obj_set_pos(heading, 8, 27);
    lv_obj_set_width(heading, 181);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    const auto transport_status =
        snapshot.transport_status ==
                lora::viewmodel::TransportStatus::WifiLan
            ? StringId::LanReady
            : (snapshot.radio_ready ? StringId::RadioReady
                                    : StringId::RadioDisabled);
    auto* radio = label(
        root,
        translate(snapshot.locale, transport_status),
        fonts.small,
        snapshot.radio_ready ? colors.accent : colors.warning);
    lv_obj_set_pos(radio, 193, 29);
    lv_obj_set_width(radio, 119);
    lv_obj_set_style_text_align(radio, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(radio, LV_LABEL_LONG_DOT);
}

void render_menu(lv_obj_t* root,
                 const lora::viewmodel::ViewSnapshot& snapshot,
                 const lora::viewmodel::MenuSnapshot& menu,
                 const LocaleFonts& fonts, const Palette& colors,
                 const std::string& logo_source) {
    auto* brand = panel(root, 7, 7, 306, 78, colors.panel,
                        colors.panel_border, 8);
    // "LORA MESSENGER" is the product's own wordmark, kept in English in
    // every language (the app's own name is not translated, matching the
    // window/app title). Rendered as a pre-generated 3D block-art PNG
    // (tools/generate_title_logo.py, same technique as the sibling
    // Battleship project's title logo) rather than an lv_label, since
    // LVGL's built-in fonts have no bevel/shading support. This replaces
    // both the old plain-text wordmark and the messenger envelope icon
    // that used to sit next to it -- the generated image *is* the title.
    if (!logo_source.empty()) {
        auto* image = lv_image_create(brand);
        lv_image_set_src(image, logo_source.c_str());
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 6);
    } else {
        // Fallback in case the PNG asset failed to resolve (e.g. missing
        // from a custom deployment) -- keep the title readable instead of
        // showing a blank image.
        auto* title = label(brand, "LORA\nMESSENGER", fonts.body, colors.text);
        lv_obj_set_width(title, 290);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(title, 2, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    }

    auto* tagline = label(
        brand, "OFFLINE BROADCAST", fonts.small, colors.accent);
    lv_obj_set_width(tagline, 290);
    lv_obj_set_style_text_align(tagline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tagline, LV_ALIGN_BOTTOM_MID, 0, -6);

    struct Entry {
        lora::viewmodel::MenuItem item;
        StringId label;
    };
    constexpr Entry entries[] = {
        {lora::viewmodel::MenuItem::Talk, StringId::MenuTalk},
        {lora::viewmodel::MenuItem::Settings,
         StringId::MenuSettings},
    };
    for (std::size_t index = 0; index < 2U; ++index) {
        const bool selected = menu.selected == entries[index].item;
        auto* row = panel(
            root, 65, 89 + static_cast<int>(index) * 25,
            190, 21, colors.panel,
            selected ? colors.accent : colors.panel_border, 4);
        lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
        std::string text = selected ? "> " : "  ";
        text += translate(snapshot.locale, entries[index].label);
        auto* row_label = label(
            row, text, fonts.body,
            selected ? colors.accent : colors.text);
        lv_obj_set_width(row_label, 176);
        lv_obj_center(row_label);
        lv_obj_set_style_text_align(
            row_label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void render_timeline(lv_obj_t* root,
                     const lora::viewmodel::ViewSnapshot& snapshot,
                     const lora::viewmodel::TimelineSnapshot& timeline,
                     const LocaleFonts& fonts, const Palette& colors) {
    if (timeline.rows.empty()) {
        auto* empty = label(root, translate(snapshot.locale, StringId::TimelineEmpty),
                            fonts.body, colors.muted_text);
        lv_obj_set_pos(empty, 14, 76);
        lv_obj_set_width(empty, 292);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (std::size_t index = 0; index < timeline.rows.size() && index < 2; ++index) {
        const auto& row = timeline.rows[index];
        const int y = 47 + static_cast<int>(index) * 45;
        auto* card = panel(root, 7, y, 306, 41, colors.panel,
                           row.selected ? colors.accent : colors.panel_border, 5);
        lv_obj_set_style_border_width(card, row.selected ? 2 : 1, 0);
        auto* sender = label(card, sender_line(row, snapshot.locale), fonts.small,
                             row.selected ? colors.accent : colors.muted_text);
        lv_obj_set_pos(sender, 6, 2);
        lv_obj_set_width(sender, 292);
        lv_label_set_long_mode(sender, LV_LABEL_LONG_DOT);
        auto* body = label(card, row.body_preview, fonts.body, colors.text);
        lv_obj_set_pos(body, 7, 18);
        lv_obj_set_width(body, 290);
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    }
}

void render_detail(lv_obj_t* root,
                   const lora::viewmodel::ViewSnapshot& snapshot,
                   const lora::viewmodel::DetailSnapshot& detail,
                   const LocaleFonts& fonts, const Palette& colors) {
    auto* card = panel(root, 7, 47, 306, 90, colors.panel,
                       colors.panel_border, 5);
    auto* sender = label(card, detail_sender_line(detail, snapshot.locale),
                         fonts.small, colors.accent);
    lv_obj_set_pos(sender, 8, 4);
    lv_obj_set_width(sender, 290);
    lv_label_set_long_mode(sender, LV_LABEL_LONG_DOT);
    auto* body = label(card, detail.visible_body, fonts.body, colors.text);
    lv_obj_set_pos(body, 8, 22);
    lv_obj_set_size(body, 290, 42);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    std::string context;
    if (detail.reply == lora::viewmodel::ReplyAvailability::Unavailable) {
        context = std::string{translate(snapshot.locale,
                                        StringId::ReplyOriginalUnavailable)};
    } else if (!detail.reply_preview.empty()) {
        context = std::string{translate(snapshot.locale,
                                        StringId::ReplyContext)} + ": ";
        context += detail.reply_preview;
    } else if (detail.mentions_me) {
        context = std::string{"@ "} +
                  std::string{translate(snapshot.locale,
                                        StringId::MentionedYou)};
    }
    auto* reply = label(card, context, fonts.small, colors.muted_text);
    lv_obj_set_pos(reply, 8, 67);
    lv_obj_set_width(reply, 290);
    lv_label_set_long_mode(reply, LV_LABEL_LONG_DOT);
}

void render_compose(lv_obj_t* root,
                    const lora::viewmodel::ViewSnapshot& snapshot,
                    const lora::viewmodel::ComposeSnapshot& compose,
                    const LocaleFonts& fonts, const Palette& colors) {
    std::string context;
    if (compose.reply_to) {
        context = std::string{translate(snapshot.locale,
                                        StringId::ReplyContext)} + "  ";
    }
    if (!compose.mentions.empty()) {
        context += "@" + std::to_string(compose.mentions.size());
    }
    if (context.empty()) {
        context = " ";
    }
    auto* context_label = label(root, context, fonts.small, colors.muted_text);
    lv_obj_set_pos(context_label, 10, 47);

    auto* editor = panel(root, 7, 62, 306, 58, colors.panel,
                         colors.accent, 5);
    std::string draft = compose.body;
    const auto cursor = std::min(compose.cursor_byte_offset, draft.size());
    draft.insert(cursor, "|");
    constexpr int kEditorViewportHeight = 48;
    auto* body_viewport = lv_obj_create(editor);
    lv_obj_remove_style_all(body_viewport);
    lv_obj_set_pos(body_viewport, 8, 5);
    lv_obj_set_size(body_viewport, 290, kEditorViewportHeight);
    lv_obj_clear_flag(body_viewport, LV_OBJ_FLAG_SCROLLABLE);

    auto* body = label(body_viewport, draft, fonts.body, colors.text);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_set_size(body, 290, LV_SIZE_CONTENT);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(body_viewport);

    lv_point_t caret_position{};
    const auto caret_character = utf8_character_count(
        std::string_view{compose.body}.substr(0, cursor));
    lv_label_get_letter_pos(body, caret_character, &caret_position);
    const int line_height = static_cast<int>(lv_font_get_line_height(fonts.body));
    const int visible_lines = std::max(1, kEditorViewportHeight / line_height);
    const int caret_line = static_cast<int>(caret_position.y) / line_height;
    const int first_visible_line = std::max(0, caret_line + 1 - visible_lines);
    lv_obj_set_y(body, -(first_visible_line * line_height));

    const std::string budget =
        std::string{translate(snapshot.locale, StringId::BytesRemaining)} +
        ": " + std::to_string(compose.remaining_bytes);
    auto* remaining = label(root, budget, fonts.small, colors.muted_text);
    lv_obj_set_pos(remaining, 168, 122);
    lv_obj_set_width(remaining, 144);
    lv_obj_set_style_text_align(remaining, LV_TEXT_ALIGN_RIGHT, 0);
}

void render_mentions(lv_obj_t* root,
                     const lora::viewmodel::ViewSnapshot& snapshot,
                     const lora::viewmodel::MentionsSnapshot& mentions,
                     const LocaleFonts& fonts, const Palette& colors) {
    if (mentions.options.empty()) {
        auto* empty = label(root, translate(snapshot.locale, StringId::NoPeers),
                            fonts.body, colors.muted_text);
        lv_obj_set_pos(empty, 14, 78);
        lv_obj_set_width(empty, 292);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    for (std::size_t index = 0; index < mentions.options.size() && index < 3; ++index) {
        const auto& option = mentions.options[index];
        const int y = 48 + static_cast<int>(index) * 29;
        auto* row = panel(root, 8, y, 304, 25, colors.panel,
                          option.cursor ? colors.accent : colors.panel_border, 4);
        std::string text = option.cursor ? "> " : "  ";
        text += option.selected ? "[x] " : "[ ] ";
        text += option.label;
        auto* row_label = label(row, text, fonts.body,
                                option.cursor ? colors.accent : colors.text);
        lv_obj_set_pos(row_label, 7, 2);
        lv_obj_set_width(row_label, 290);
        lv_label_set_long_mode(row_label, LV_LABEL_LONG_DOT);
    }
}

void render_settings(lv_obj_t* root,
                     const lora::viewmodel::ViewSnapshot& snapshot,
                     const lora::viewmodel::SettingsSnapshot& settings,
                     const LocaleFonts& fonts, const Palette& colors) {
    auto* card = panel(root, 7, 45, 306, 92, colors.panel,
                       colors.panel_border, 5);
    std::string user = std::string{translate(snapshot.locale,
                                             StringId::SettingsUserId)} +
                       ": " + settings.user_id;
    auto* user_label = label(card, user, fonts.small, colors.text);
    lv_obj_set_pos(user_label, 8, 2);
    lv_obj_set_width(user_label, 290);
    lv_label_set_long_mode(user_label, LV_LABEL_LONG_DOT);

    std::string identity =
        std::string{translate(snapshot.locale, StringId::SettingsIdentity)} +
        ": #" + settings.install_suffix;
    auto* id_label = label(card, identity, fonts.small, colors.muted_text);
    lv_obj_set_pos(id_label, 8, 17);

    const bool language_selected =
        settings.selected == lora::viewmodel::SettingsItem::Language;
    std::string language = language_selected ? "> < " : "    ";
    language +=
        std::string{translate(snapshot.locale, StringId::SettingsLanguage)} +
        ": " + settings.locale_name;
    if (language_selected) {
        language += " >";
    }
    auto* language_label = label(
        card, language, fonts.body,
        language_selected ? colors.accent : colors.text);
    lv_obj_set_pos(language_label, 8, 32);
    lv_obj_set_width(language_label, 290);
    lv_label_set_long_mode(language_label, LV_LABEL_LONG_DOT);

    const bool skip_selected =
        settings.selected == lora::viewmodel::SettingsItem::SkipTitle;
    std::string skip = skip_selected ? "> < " : "    ";
    skip += translate(snapshot.locale, StringId::SettingsSkipTitle);
    skip += ": ";
    skip += translate(
        snapshot.locale,
        settings.skip_title ? StringId::ToggleOn
                            : StringId::ToggleOff);
    if (skip_selected) {
        skip += " >";
    }
    auto* skip_label = label(
        card, skip, fonts.body,
        skip_selected ? colors.accent : colors.text);
    lv_obj_set_pos(skip_label, 8, 51);
    lv_obj_set_width(skip_label, 290);
    lv_label_set_long_mode(skip_label, LV_LABEL_LONG_DOT);

    auto* radio_label = label(card, settings.radio_status, fonts.small,
                              colors.warning);
    lv_obj_set_pos(radio_label, 8, 72);
    lv_obj_set_width(radio_label, 290);
    lv_label_set_long_mode(radio_label, LV_LABEL_LONG_DOT);
}

void render_modal(lv_obj_t* root,
                  const lora::viewmodel::ViewSnapshot& snapshot,
                  const LocaleFonts& fonts, const Palette& colors) {
    if (snapshot.modal.id == ModalId::None) {
        return;
    }
    auto* shade = lv_obj_create(root);
    lv_obj_remove_style_all(shade);
    lv_obj_set_pos(shade, 0, kHeaderHeight);
    lv_obj_set_size(shade, kScreenWidth, kScreenHeight - kHeaderHeight);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
    lv_obj_clear_flag(shade, LV_OBJ_FLAG_SCROLLABLE);

    const bool error = snapshot.modal.id == ModalId::Error;
    auto* modal = panel(shade, 15, 9, 290, 109, colors.panel,
                        error ? colors.warning : colors.accent, 7);
    auto* title = label(modal, snapshot.modal.title, fonts.body,
                        error ? colors.warning : colors.accent);
    lv_obj_set_pos(title, 10, 8);
    lv_obj_set_width(title, 270);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    auto* message = label(modal, snapshot.modal.message, fonts.small, colors.text);
    lv_obj_set_pos(message, 10, 31);
    lv_obj_set_size(message, 270, 47);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);

    std::string actions;
    if (snapshot.modal.id == ModalId::Discard ||
        snapshot.modal.id == ModalId::Exit ||
        snapshot.modal.id == ModalId::DeleteData ||
        snapshot.modal.id == ModalId::Recovery) {
        actions = snapshot.modal.confirm_selected ? "  [" : "> [";
        actions += snapshot.modal.cancel_label;
        actions += snapshot.modal.confirm_selected ? "]   > [" : "]     [";
        actions += snapshot.modal.confirm_label;
        actions += "]";
    } else {
        actions = "> [Enter / Esc]";
    }
    auto* action_label = label(modal, actions, fonts.small, colors.text);
    lv_obj_set_pos(action_label, 10, 84);
    lv_obj_set_width(action_label, 270);
    lv_obj_set_style_text_align(action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(action_label, LV_LABEL_LONG_DOT);
}

} // namespace

bool MessengerFonts::complete() const noexcept {
    return english.body && english.small && japanese.body && japanese.small &&
           simplified_chinese.body && simplified_chinese.small;
}

MessengerScreen::MessengerScreen(
    MessengerFonts fonts, std::filesystem::path logo_path)
    : fonts_(fonts),
      logo_source_(logo_path.empty()
                       ? std::string{}
                       : std::string{"A:"} + logo_path.string()) {}

MessengerScreen::~MessengerScreen() {
    shutdown();
}

bool MessengerScreen::show(const lora::viewmodel::ViewSnapshot& snapshot) {
    if (!root_) {
        root_ = lv_obj_create(nullptr);
        if (!root_) {
            return false;
        }
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, kScreenWidth, kScreenHeight);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_screen_load(root_);
    }
    return render(snapshot);
}

bool MessengerScreen::render(const lora::viewmodel::ViewSnapshot& snapshot) {
    if (!root_ || !lv_is_initialized() || !lv_obj_is_valid(root_)) {
        return false;
    }
    const auto colors = app_palette();
    const auto& configured = fonts_for(snapshot.locale);
    LocaleFonts fonts{
        usable_font(configured.body, &lv_font_montserrat_16),
        usable_font(configured.small, &lv_font_montserrat_14),
    };

    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, colors.background, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    if (snapshot.screen == lora::viewmodel::ScreenId::Menu) {
        const auto* menu =
            std::get_if<lora::viewmodel::MenuSnapshot>(
                &snapshot.page);
        if (!menu) {
            return false;
        }
        render_menu(
            root_, snapshot, *menu, fonts, colors, logo_source_);
        render_footer(root_, snapshot, fonts, colors);
        render_modal(root_, snapshot, fonts, colors);
        lv_obj_invalidate(root_);
        return true;
    }
    render_header(root_, snapshot, fonts, colors);
    render_page_heading(root_, snapshot, fonts, colors);

    std::visit([&](const auto& page) {
        using Page = std::decay_t<decltype(page)>;
        if constexpr (std::is_same_v<Page, lora::viewmodel::MenuSnapshot>) {
            // Menu is rendered before the standard header/page path.
        } else if constexpr (std::is_same_v<Page, lora::viewmodel::TimelineSnapshot>) {
            render_timeline(root_, snapshot, page, fonts, colors);
        } else if constexpr (std::is_same_v<Page, lora::viewmodel::DetailSnapshot>) {
            render_detail(root_, snapshot, page, fonts, colors);
        } else if constexpr (std::is_same_v<Page, lora::viewmodel::ComposeSnapshot>) {
            render_compose(root_, snapshot, page, fonts, colors);
        } else if constexpr (std::is_same_v<Page, lora::viewmodel::MentionsSnapshot>) {
            render_mentions(root_, snapshot, page, fonts, colors);
        } else if constexpr (std::is_same_v<Page, lora::viewmodel::SettingsSnapshot>) {
            render_settings(root_, snapshot, page, fonts, colors);
        }
    }, snapshot.page);

    render_footer(root_, snapshot, fonts, colors);
    render_modal(root_, snapshot, fonts, colors);
    lv_obj_invalidate(root_);
    return true;
}

void MessengerScreen::shutdown() {
    if (!root_) {
        return;
    }
    if (lv_is_initialized() && lv_obj_is_valid(root_)) {
        if (lv_screen_active() == root_) {
            auto* blank = lv_obj_create(nullptr);
            lv_screen_load(blank);
        }
        lv_obj_delete(root_);
    }
    root_ = nullptr;
}

lv_obj_t* MessengerScreen::root() const noexcept {
    return root_;
}

const LocaleFonts& MessengerScreen::fonts_for(Locale locale) const noexcept {
    switch (locale) {
        case Locale::English: return fonts_.english;
        case Locale::Japanese: return fonts_.japanese;
        case Locale::SimplifiedChinese: return fonts_.simplified_chinese;
        case Locale::Count: break;
    }
    return fonts_.english;
}

} // namespace view
