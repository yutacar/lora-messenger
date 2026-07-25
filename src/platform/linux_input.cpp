/*
 * SPDX-License-Identifier: MIT
 */

#include "linux_input.h"

#include "key_codec.h"

#include <cstdint>
#include <cstring>
#include <utility>

#if !USE_DESKTOP
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifndef APP_KEY_INPUT_DEVICE
#define APP_KEY_INPUT_DEVICE ""
#endif

namespace platform {
namespace {

KeyHandler key_handler;
std::uint32_t last_key = 0;
bool last_key_pressed = false;

void key_event_cb(lv_event_t* event) {
    LV_UNUSED(event);
    auto* input = lv_indev_active();
    if (!input) {
        return;
    }

    // lv_indev_get_key() returns whatever the active indev's read callback
    // put in data->key. For the SDL keyboard indev (lv_sdl_keyboard.c),
    // that is a whole UTF-8-encoded character packed byte-for-byte into the
    // uint32_t -- not a codepoint -- for anything delivered via
    // SDL_TEXTINPUT, which is how the host OS's native IME delivers
    // composed Japanese/Chinese input. decode_packed_utf8_key() unpacks
    // that back into the real codepoint (ASCII and the evdev keypad's own
    // single-byte keys round-trip unchanged); every downstream consumer of
    // route_key() -- including app_script.cpp's TEXT= injection -- already
    // assumes it is handed a real codepoint, so this is the one place that
    // needs to do the unpacking.
    const auto key = decode_packed_utf8_key(lv_indev_get_key(input));
    const bool pressed = lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
    if (pressed && (!last_key_pressed || last_key != key)) {
        route_key(key);
    }
    last_key = key;
    last_key_pressed = pressed;
}

#if !USE_DESKTOP
struct EvdevKeypad {
    int fd{-1};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
    std::uint32_t key{0};
    bool left_shift{false};
    bool right_shift{false};
    bool caps_lock{false};
};

char shifted_letter(char letter, bool shift, bool caps_lock) noexcept {
    if (shift != caps_lock) {
        return static_cast<char>(letter - ('a' - 'A'));
    }
    return letter;
}

std::uint32_t ascii_key(char character) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(character));
}

std::uint32_t map_letter(std::uint16_t code, bool shift, bool caps_lock) noexcept {
    char letter = 0;
    switch (code) {
        case KEY_A: letter = 'a'; break;
        case KEY_B: letter = 'b'; break;
        case KEY_C: letter = 'c'; break;
        case KEY_D: letter = 'd'; break;
        case KEY_E: letter = 'e'; break;
        case KEY_F: letter = 'f'; break;
        case KEY_G: letter = 'g'; break;
        case KEY_H: letter = 'h'; break;
        case KEY_I: letter = 'i'; break;
        case KEY_J: letter = 'j'; break;
        case KEY_K: letter = 'k'; break;
        case KEY_L: letter = 'l'; break;
        case KEY_M: letter = 'm'; break;
        case KEY_N: letter = 'n'; break;
        case KEY_O: letter = 'o'; break;
        case KEY_P: letter = 'p'; break;
        case KEY_Q: letter = 'q'; break;
        case KEY_R: letter = 'r'; break;
        case KEY_S: letter = 's'; break;
        case KEY_T: letter = 't'; break;
        case KEY_U: letter = 'u'; break;
        case KEY_V: letter = 'v'; break;
        case KEY_W: letter = 'w'; break;
        case KEY_X: letter = 'x'; break;
        case KEY_Y: letter = 'y'; break;
        case KEY_Z: letter = 'z'; break;
        default: return 0;
    }
    return ascii_key(shifted_letter(letter, shift, caps_lock));
}

std::uint32_t map_number(std::uint16_t code, bool shift) noexcept {
    switch (code) {
        case KEY_1: return ascii_key(shift ? '!' : '1');
        case KEY_2: return ascii_key(shift ? '@' : '2');
        case KEY_3: return ascii_key(shift ? '#' : '3');
        case KEY_4: return ascii_key(shift ? '$' : '4');
        case KEY_5: return ascii_key(shift ? '%' : '5');
        case KEY_6: return ascii_key(shift ? '^' : '6');
        case KEY_7: return ascii_key(shift ? '&' : '7');
        case KEY_8: return ascii_key(shift ? '*' : '8');
        case KEY_9: return ascii_key(shift ? '(' : '9');
        case KEY_0: return ascii_key(shift ? ')' : '0');
        default: return 0;
    }
}

std::uint32_t map_evdev_key(std::uint16_t code, bool shift,
                            bool caps_lock) noexcept {
    if (const auto letter = map_letter(code, shift, caps_lock)) {
        return letter;
    }
    if (const auto number = map_number(code, shift)) {
        return number;
    }
    switch (code) {
        case KEY_HOME: return LV_KEY_HOME;
        case KEY_ESC: return LV_KEY_ESC;
        case KEY_ENTER: return LV_KEY_ENTER;
        case KEY_UP: return LV_KEY_UP;
        case KEY_DOWN: return LV_KEY_DOWN;
        case KEY_LEFT: return LV_KEY_LEFT;
        case KEY_RIGHT: return LV_KEY_RIGHT;
        case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
        case KEY_DELETE: return LV_KEY_DEL;
        case KEY_TAB: return LV_KEY_NEXT;
        case KEY_SPACE: return ascii_key(' ');
        case KEY_MINUS: return ascii_key(shift ? '_' : '-');
        case KEY_EQUAL: return ascii_key(shift ? '+' : '=');
        case KEY_LEFTBRACE: return ascii_key(shift ? '{' : '[');
        case KEY_RIGHTBRACE: return ascii_key(shift ? '}' : ']');
        case KEY_BACKSLASH: return ascii_key(shift ? '|' : '\\');
        case KEY_SEMICOLON: return ascii_key(shift ? ':' : ';');
        case KEY_APOSTROPHE: return ascii_key(shift ? '"' : '\'');
        case KEY_GRAVE: return ascii_key(shift ? '~' : '`');
        case KEY_COMMA: return ascii_key(shift ? '<' : ',');
        case KEY_DOT: return ascii_key(shift ? '>' : '.');
        case KEY_SLASH: return ascii_key(shift ? '?' : '/');
        default: return 0;
    }
}

bool update_modifier(EvdevKeypad& keypad, const input_event& event) noexcept {
    const bool pressed = event.value != 0;
    switch (event.code) {
        case KEY_LEFTSHIFT:
            keypad.left_shift = pressed;
            return true;
        case KEY_RIGHTSHIFT:
            keypad.right_shift = pressed;
            return true;
        case KEY_CAPSLOCK:
            if (event.value == 1) {
                keypad.caps_lock = !keypad.caps_lock;
            }
            return true;
        default:
            return false;
    }
}

bool supports_keyboard_keys(int fd) {
    unsigned long bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        return false;
    }
    const auto has_key = [&](int code) {
        const auto width = static_cast<int>(sizeof(unsigned long) * 8);
        return (bits[code / width] & (1UL << (code % width))) != 0;
    };
    return has_key(KEY_HOME) || has_key(KEY_ESC) || has_key(KEY_N);
}

void evdev_read_cb(lv_indev_t* input, lv_indev_data_t* data) {
    auto* keypad = static_cast<EvdevKeypad*>(lv_indev_get_driver_data(input));
    if (!keypad) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    input_event event{};
    while (read(keypad->fd, &event, sizeof(event)) == sizeof(event)) {
        if (event.type != EV_KEY) {
            continue;
        }
        if (update_modifier(*keypad, event)) {
            data->continue_reading = true;
            continue;
        }
        if (event.value == 2) {
            continue;
        }
        const bool shift = keypad->left_shift || keypad->right_shift;
        const auto key = map_evdev_key(event.code, shift, keypad->caps_lock);
        if (!key) {
            continue;
        }
        keypad->key = key;
        keypad->state = event.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = true;
        break;
    }
    data->key = keypad->key;
    data->state = keypad->state;
}

void evdev_delete_cb(lv_event_t* event) {
    auto* input = static_cast<lv_indev_t*>(lv_event_get_target(event));
    auto* keypad = static_cast<EvdevKeypad*>(lv_indev_get_driver_data(input));
    if (!keypad) {
        return;
    }
    if (keypad->fd >= 0) {
        close(keypad->fd);
    }
    delete keypad;
}

lv_indev_t* create_keypad(int fd) {
    auto* keypad = new EvdevKeypad;
    keypad->fd = fd;
    auto* input = lv_indev_create();
    if (!input) {
        close(fd);
        delete keypad;
        return nullptr;
    }
    lv_indev_set_type(input, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(input, evdev_read_cb);
    lv_indev_set_driver_data(input, keypad);
    lv_indev_add_event_cb(input, evdev_delete_cb, LV_EVENT_DELETE, nullptr);
    attach_key_router(input);
    return input;
}

lv_indev_t* try_keypad(const char* path) {
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    if (!supports_keyboard_keys(fd)) {
        close(fd);
        return nullptr;
    }
    LV_LOG_INFO("using evdev key input %s", path);
    return create_keypad(fd);
}

void discover_keypads(lv_display_t* display) {
    if (APP_KEY_INPUT_DEVICE[0] != '\0') {
        if (auto* input = try_keypad(APP_KEY_INPUT_DEVICE)) {
            lv_indev_set_display(input, display);
        }
        return;
    }

    auto* directory = opendir("/dev/input");
    if (!directory) {
        LV_LOG_WARN("failed to open /dev/input: %s", std::strerror(errno));
        return;
    }
    while (auto* entry = readdir(directory)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        const std::string path = std::string{"/dev/input/"} + entry->d_name;
        if (auto* input = try_keypad(path.c_str())) {
            lv_indev_set_display(input, display);
        }
    }
    closedir(directory);
}
#endif

} // namespace

void init_key_input(lv_display_t* display) {
#if USE_DESKTOP
    LV_UNUSED(display);
#else
    discover_keypads(display);
#endif
}

void attach_key_router(lv_indev_t* input) {
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }
    lv_indev_add_event_cb(input, key_event_cb, LV_EVENT_KEY, nullptr);
}

void set_key_handler(KeyHandler handler) {
    key_handler = std::move(handler);
    last_key = 0;
    last_key_pressed = false;
}

void route_key(std::uint32_t key) {
    if (key_handler) {
        key_handler(key);
    }
}

} // namespace platform
