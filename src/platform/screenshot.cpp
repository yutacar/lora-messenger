/*
 * SPDX-License-Identifier: MIT
 */

#include "screenshot.h"

#include "app_script_parser.h"
#include "logger.h"
#include "lvgl.h"

#if USE_DESKTOP && LV_USE_SNAPSHOT
#include "src/draw/snapshot/lv_snapshot.h"

#include <png.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#endif

namespace platform {

#if USE_DESKTOP && LV_USE_SNAPSHOT
namespace {

bool write_png(const std::filesystem::path& path, const lv_draw_buf_t& buffer) {
    std::vector<std::uint8_t> row(static_cast<std::size_t>(buffer.header.w) * 4U);
    auto* file = std::fopen(path.string().c_str(), "wb");
    if (!file) {
        return false;
    }
    auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    auto* info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) {
            png_destroy_write_struct(&png, info ? &info : nullptr);
        }
        std::fclose(file);
        return false;
    }

    png_init_io(png, file);
    png_set_IHDR(png, info, buffer.header.w, buffer.header.h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (std::uint32_t y = 0; y < buffer.header.h; ++y) {
        const auto* source = buffer.data + static_cast<std::size_t>(y) * buffer.header.stride;
        for (std::uint32_t x = 0; x < buffer.header.w; ++x) {
            row[x * 4U + 0U] = source[x * 4U + 2U];
            row[x * 4U + 1U] = source[x * 4U + 1U];
            row[x * 4U + 2U] = source[x * 4U + 0U];
            row[x * 4U + 3U] = source[x * 4U + 3U];
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return std::fclose(file) == 0;
}

std::filesystem::path temporary_path_for(const std::filesystem::path& final_path) {
    static std::atomic_uint64_t sequence{0};
#ifdef _WIN32
    const auto process_id = static_cast<unsigned long>(_getpid());
#else
    const auto process_id = static_cast<unsigned long>(getpid());
#endif
    auto temporary = final_path;
    temporary += ".tmp-" + std::to_string(process_id) + "-" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

} // namespace

bool capture_screenshot(const std::string& stem) {
    if (stem.empty()) {
        LOG_ERROR("screenshot name is empty after sanitizing");
        return false;
    }
    if (!script::is_safe_screenshot_stem(stem)) {
        LOG_ERROR("screenshot name is not a canonical safe lowercase stem");
        return false;
    }
    auto* display = lv_display_get_default();
    auto* screen = lv_screen_active();
    if (!display || !screen) {
        LOG_ERROR("screenshot requested without an active display");
        return false;
    }

    lv_refr_now(display);
    auto* buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    if (!buffer) {
        LOG_ERROR("LVGL snapshot failed");
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(kScreenshotDirectory, error);
    const auto path = std::filesystem::path{kScreenshotDirectory} / (stem + ".png");
    const auto temporary_path = temporary_path_for(path);
    bool saved = false;
    if (!error && write_png(temporary_path, *buffer)) {
        std::filesystem::rename(temporary_path, path, error);
        saved = !error;
    }
    if (!saved) {
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
    }
    lv_draw_buf_destroy(buffer);
    if (saved) {
        LOG_INFO("screenshot saved: {}", path.string());
    } else {
        LOG_ERROR("failed to save screenshot: {}", path.string());
    }
    return saved;
}

#else

bool capture_screenshot(const std::string& stem) {
    LV_UNUSED(stem);
    LOG_WARN("screenshot capture is unavailable in this build");
    return false;
}

#endif

} // namespace platform
