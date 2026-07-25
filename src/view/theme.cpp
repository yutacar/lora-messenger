/*
 * SPDX-License-Identifier: MIT
 */

#include "theme.h"

namespace view {

Palette app_palette() {
    return {
        lv_color_hex(0x0B1020),
        lv_color_hex(0x151D31),
        lv_color_hex(0x2B3855),
        lv_color_hex(0xF4F7FF),
        lv_color_hex(0x9AA8C2),
        lv_color_hex(0x65D6B4),
        lv_color_hex(0xFFB454),
        lv_color_hex(0x0F1728),
    };
}

void apply_lvgl_theme(lv_display_t* display) {
    if (!display) {
        return;
    }
    const auto palette = app_palette();
    auto* theme = lv_theme_default_init(display, palette.accent, palette.warning, true, LV_FONT_DEFAULT);
    lv_display_set_theme(display, theme);
}

} // namespace view
