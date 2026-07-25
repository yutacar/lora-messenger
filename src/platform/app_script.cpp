/*
 * SPDX-License-Identifier: MIT
 */

#include "app_script.h"

#include "app_script_parser.h"
#include "linux_input.h"
#include "screenshot.h"
#include "lvgl.h"

#if USE_DESKTOP
#include "src/drivers/sdl/lv_sdl_window.h"
#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace platform {

#if USE_DESKTOP
namespace {

struct ScriptState {
    std::vector<script::Action> actions;
    std::size_t next{0};
    std::uint32_t await_ticks{0};
    AppScriptProbe probe;
};

lv_timer_t* script_timer = nullptr;
ScriptState* script_state = nullptr;
bool script_failed = false;

void release_script(bool delete_timer) {
    auto* timer = script_timer;
    script_timer = nullptr;
    if (delete_timer && timer && lv_is_initialized()) {
        lv_timer_delete(timer);
    }
    delete script_state;
    script_state = nullptr;
}

void fail_script(const char* diagnostic) {
    script_failed = true;
    LV_LOG_ERROR("%s", diagnostic);
    release_script(true);
}

std::uint32_t route_value(const script::Action& action) noexcept {
    switch (action.named_key) {
        case script::NamedKey::Home: return LV_KEY_HOME;
        case script::NamedKey::Escape: return LV_KEY_ESC;
        case script::NamedKey::Up: return LV_KEY_UP;
        case script::NamedKey::Down: return LV_KEY_DOWN;
        case script::NamedKey::Left: return LV_KEY_LEFT;
        case script::NamedKey::Right: return LV_KEY_RIGHT;
        case script::NamedKey::Enter: return LV_KEY_ENTER;
        case script::NamedKey::Backspace: return LV_KEY_BACKSPACE;
        case script::NamedKey::Tab: return LV_KEY_NEXT;
        case script::NamedKey::None: return action.scalar;
    }
    return 0;
}

bool request_window_close() {
    auto* display = lv_display_get_default();
    auto* window = display ? lv_sdl_window_get_window(display) : nullptr;
    if (!window) {
        LV_LOG_ERROR("APP_SCRIPT CLOSE requested without an SDL window");
        return false;
    }

    const auto window_id = SDL_GetWindowID(window);
    if (window_id == 0U) {
        LV_LOG_ERROR("APP_SCRIPT CLOSE could not resolve the SDL window id");
        return false;
    }
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.type = SDL_WINDOWEVENT;
    event.window.windowID = window_id;
    event.window.event = SDL_WINDOWEVENT_CLOSE;
    if (SDL_PushEvent(&event) != 1) {
        LV_LOG_ERROR("APP_SCRIPT CLOSE could not enqueue the SDL window event");
        return false;
    }
    return true;
}

enum class ProbeResult {
    Match,
    Mismatch,
    Failure,
};

ProbeResult evaluate_probe(const script::Action& action) {
    if (!script_state || !script_state->probe) {
        LV_LOG_ERROR("APP_SCRIPT probe is unavailable for field %s",
                     action.field.c_str());
        return ProbeResult::Failure;
    }
    try {
        const auto current = script_state->probe(action.field);
        if (!current) {
            LV_LOG_ERROR("APP_SCRIPT probe field is unavailable: %s",
                         action.field.c_str());
            return ProbeResult::Failure;
        }
        if (*current == action.value) {
            return ProbeResult::Match;
        }
        return ProbeResult::Mismatch;
    } catch (const std::exception&) {
        LV_LOG_ERROR("APP_SCRIPT probe threw an exception for field %s",
                     action.field.c_str());
        return ProbeResult::Failure;
    } catch (...) {
        LV_LOG_ERROR("APP_SCRIPT probe threw a non-standard exception for field %s",
                     action.field.c_str());
        return ProbeResult::Failure;
    }
}

void log_expectation_mismatch(const script::Action& action, bool timed_out) {
    if (timed_out) {
        LV_LOG_ERROR("APP_SCRIPT AWAIT timed out for %s: expected %s",
                     action.field.c_str(), action.value.c_str());
    } else {
        LV_LOG_ERROR("APP_SCRIPT EXPECT failed for %s: expected %s",
                     action.field.c_str(), action.value.c_str());
    }
}

void complete_script() {
    LV_LOG_USER("APP_SCRIPT completed successfully");
    release_script(true);
}

void run_script_action() {
    if (!script_state || script_state->next >= script_state->actions.size()) {
        complete_script();
        return;
    }

    const auto& action = script_state->actions[script_state->next];
    switch (action.type) {
        case script::ActionType::Key:
            route_key(route_value(action));
            ++script_state->next;
            script_state->await_ticks = 0;
            break;
        case script::ActionType::Wait:
            ++script_state->next;
            script_state->await_ticks = 0;
            break;
        case script::ActionType::Screenshot:
            if (!capture_screenshot(action.value)) {
                fail_script("APP_SCRIPT screenshot action failed");
                return;
            }
            ++script_state->next;
            script_state->await_ticks = 0;
            break;
        case script::ActionType::Expect: {
            const auto evaluation = evaluate_probe(action);
            if (evaluation == ProbeResult::Failure) {
                fail_script("APP_SCRIPT expectation probe failed");
                return;
            }
            if (evaluation == ProbeResult::Mismatch) {
                log_expectation_mismatch(action, false);
                fail_script("APP_SCRIPT expectation mismatch");
                return;
            }
            ++script_state->next;
            script_state->await_ticks = 0;
            break;
        }
        case script::ActionType::Await: {
            const auto evaluation = evaluate_probe(action);
            if (evaluation == ProbeResult::Failure) {
                fail_script("APP_SCRIPT await probe failed");
                return;
            }
            if (evaluation == ProbeResult::Mismatch) {
                ++script_state->await_ticks;
                if (script_state->await_ticks >= script::kMaximumAwaitTicks) {
                    log_expectation_mismatch(action, true);
                    fail_script("APP_SCRIPT await timed out");
                }
                return;
            }
            ++script_state->next;
            script_state->await_ticks = 0;
            break;
        }
        case script::ActionType::CloseWindow:
            if (!request_window_close()) {
                fail_script("APP_SCRIPT CLOSE action failed");
                return;
            }
            ++script_state->next;
            complete_script();
            return;
    }

    if (script_state && script_state->next >= script_state->actions.size()) {
        complete_script();
    }
}

void script_timer_cb(lv_timer_t* timer) {
    LV_UNUSED(timer);
    try {
        run_script_action();
    } catch (const std::exception&) {
        fail_script("APP_SCRIPT action threw an exception");
    } catch (...) {
        fail_script("APP_SCRIPT action threw a non-standard exception");
    }
}

} // namespace
#endif

void start_app_script() {
    start_app_script({});
}

void start_app_script(AppScriptProbe probe) {
#if USE_DESKTOP
    stop_app_script();
    script_failed = false;
    const char* value = std::getenv("APP_SCRIPT");
    if (!value || value[0] == '\0') {
        return;
    }

    try {
        std::size_t source_size = 0;
        while (source_size <= script::kMaximumSourceBytes &&
               value[source_size] != '\0') {
            ++source_size;
        }
        if (source_size > script::kMaximumSourceBytes) {
            script_failed = true;
            LV_LOG_ERROR("APP_SCRIPT parse error at token 0: script source exceeds 16384 bytes");
            return;
        }
        auto parsed = script::parse(std::string_view{value, source_size});
        if (!parsed) {
            script_failed = true;
            LV_LOG_ERROR("APP_SCRIPT parse error at token %zu: %s",
                         parsed.token_index, parsed.diagnostic.c_str());
            return;
        }
        if (parsed.actions.empty()) {
            script_failed = true;
            LV_LOG_ERROR("APP_SCRIPT did not contain an action");
            return;
        }

        const auto interval =
            script::parse_interval(std::getenv("APP_SCRIPT_INTERVAL_MS"));
        if (!interval.valid) {
            script_failed = true;
            LV_LOG_ERROR("%s", interval.diagnostic.c_str());
            return;
        }

        script_state = new ScriptState{
            std::move(parsed.actions), 0, 0, std::move(probe),
        };
        script_timer = lv_timer_create(script_timer_cb, interval.milliseconds, nullptr);
        if (!script_timer) {
            delete script_state;
            script_state = nullptr;
            script_failed = true;
            LV_LOG_ERROR("APP_SCRIPT failed to create its timer");
            return;
        }
        LV_LOG_USER("APP_SCRIPT started with %zu actions", script_state->actions.size());
    } catch (const std::exception&) {
        release_script(true);
        script_failed = true;
        LV_LOG_ERROR("APP_SCRIPT failed during bounded setup");
    } catch (...) {
        release_script(true);
        script_failed = true;
        LV_LOG_ERROR("APP_SCRIPT failed during bounded setup with a non-standard exception");
    }
#else
    static_cast<void>(probe);
#endif
}

void stop_app_script() {
#if USE_DESKTOP
    release_script(true);
#endif
}

bool app_script_failed() {
#if USE_DESKTOP
    return script_failed;
#else
    return false;
#endif
}

bool app_script_pending() {
#if USE_DESKTOP
    return script_state != nullptr;
#else
    return false;
#endif
}

} // namespace platform
