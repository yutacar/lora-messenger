/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "viewmodel/messenger_view_model.h"

#include "lvgl.h"

#include <filesystem>
#include <string>

namespace view {

struct LocaleFonts {
    const lv_font_t* body{nullptr};
    const lv_font_t* small{nullptr};
};

struct MessengerFonts {
    LocaleFonts english;
    LocaleFonts japanese;
    LocaleFonts simplified_chinese;

    bool complete() const noexcept;
};

class MessengerScreen {
public:
    explicit MessengerScreen(
        MessengerFonts fonts,
        std::filesystem::path logo_path = {});
    ~MessengerScreen();

    MessengerScreen(const MessengerScreen&) = delete;
    MessengerScreen& operator=(const MessengerScreen&) = delete;

    bool show(const lora::viewmodel::ViewSnapshot& snapshot);
    bool render(const lora::viewmodel::ViewSnapshot& snapshot);
    void shutdown();
    lv_obj_t* root() const noexcept;

private:
    const LocaleFonts& fonts_for(lora::viewmodel::Locale locale) const noexcept;

    MessengerFonts fonts_;
    std::string logo_source_;
    lv_obj_t* root_{nullptr};
};

} // namespace view
