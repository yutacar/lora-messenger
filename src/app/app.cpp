/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "app_script.h"
#include "asset_manager.h"
#include "core/app_metadata.h"
#include "demo_scenario.h"
#include "linux_input.h"
#include "logger.h"
#include "messenger_screen.h"
#include "persistent_session.h"
#include "radio_runtime.h"
#include "theme.h"
#include "viewmodel/i18n.h"
#include "viewmodel/messenger_view_model.h"

#if !USE_DESKTOP
#include "adapters/network/lan_broadcast_policy.h"
#include "adapters/network/posix_udp_broadcast_socket.h"
#include "adapters/network/udp_broadcast_transport.h"
#include "adapters/radio/cap_lora_1262_radio.h"
#include "adapters/radio/japan_920_radio_policy.h"
#include "adapters/radio/linux_cap_lora_1262_radio.h"
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif
#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif
#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#include <optional>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace app {
namespace {

struct DisplayResources {
    lv_display_t* display{nullptr};
#if USE_DESKTOP
    lv_indev_t* mouse{nullptr};
    lv_indev_t* mouse_wheel{nullptr};
    lv_indev_t* keyboard{nullptr};
#endif
};

struct RunState {
    bool running{true};
    int exit_code{0};
    DisplayResources resources;

    void request_stop() noexcept {
        running = false;
    }

    void request_failure() noexcept {
        exit_code = 1;
        running = false;
    }
};

void display_deleted_cb(lv_event_t* event) {
    auto* state = static_cast<RunState*>(lv_event_get_user_data(event));
    if (state) {
        state->resources.display = nullptr;
        state->request_stop();
    }
}

DisplayResources init_display() {
    DisplayResources resources;
#if USE_DESKTOP
    resources.display = lv_sdl_window_create(view::kScreenWidth, view::kScreenHeight);
    if (!resources.display) {
        return resources;
    }
    lv_sdl_window_set_title(resources.display, lora::core::kDisplayName);
    lv_sdl_window_set_resizeable(resources.display, false);
    resources.mouse = lv_sdl_mouse_create();
    resources.mouse_wheel = lv_sdl_mousewheel_create();
    resources.keyboard = lv_sdl_keyboard_create();
    platform::attach_key_router(resources.keyboard);
#elif APP_USE_DRM
    resources.display = lv_linux_drm_create();
    if (resources.display &&
        lv_linux_drm_set_file(resources.display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(resources.display);
        resources.display = nullptr;
    }
    if (resources.display) {
        platform::init_key_input(resources.display);
    }
#else
    resources.display = lv_linux_fbdev_create();
    if (resources.display &&
        lv_linux_fbdev_set_file(resources.display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(resources.display);
        resources.display = nullptr;
    }
    if (resources.display) {
        platform::init_key_input(resources.display);
    }
#endif
    return resources;
}

void delete_input(lv_indev_t*& input) {
    if (input && lv_is_initialized()) {
        lv_indev_delete(input);
    }
    input = nullptr;
}

void shutdown_display(DisplayResources& resources) {
    if (!lv_is_initialized()) {
        resources.display = nullptr;
        return;
    }
#if USE_DESKTOP
    delete_input(resources.keyboard);
    delete_input(resources.mouse_wheel);
    delete_input(resources.mouse);
#endif
    if (resources.display) {
        auto* display = resources.display;
        resources.display = nullptr;
        lv_display_delete(display);
    }
#if USE_DESKTOP
    lv_sdl_quit();
#endif
    if (lv_is_initialized()) {
        lv_deinit();
    }
}

lora::viewmodel::KeyEvent semantic_key(std::uint32_t key) noexcept {
    using lora::viewmodel::KeyEvent;
    using lora::viewmodel::UiKey;
    switch (key) {
        case LV_KEY_UP: return {UiKey::Up, 0};
        case LV_KEY_DOWN: return {UiKey::Down, 0};
        case LV_KEY_LEFT: return {UiKey::Left, 0};
        case LV_KEY_RIGHT: return {UiKey::Right, 0};
        case LV_KEY_ENTER: return {UiKey::Enter, 0};
        case LV_KEY_ESC: return {UiKey::Escape, 0};
        case LV_KEY_HOME: return {UiKey::Home, 0};
        case LV_KEY_BACKSPACE: return {UiKey::Backspace, 0};
        case LV_KEY_DEL: return {UiKey::Delete, 0};
        case LV_KEY_NEXT: return {UiKey::Tab, 0};
        // By the time a key reaches here it is always a real Unicode
        // codepoint: platform::route_key()'s two callers already normalize
        // to that -- linux_input.cpp's key_event_cb() unpacks LVGL's
        // packed-UTF-8 indev key values (see key_codec.h), and
        // app_script.cpp's TEXT= handling decodes percent-encoded UTF-8
        // into codepoints itself. Neither ever hands this a packed value.
        default: return {UiKey::Character, static_cast<char32_t>(key)};
    }
}

std::string screen_probe(lora::viewmodel::ScreenId screen) {
    using lora::viewmodel::ScreenId;
    switch (screen) {
        case ScreenId::Menu: return "menu";
        case ScreenId::Timeline: return "timeline";
        case ScreenId::Detail: return "detail";
        case ScreenId::Compose: return "compose";
        case ScreenId::Mentions: return "mentions";
        case ScreenId::Settings: return "settings";
    }
    return "unknown";
}

std::string modal_probe(lora::viewmodel::ModalId modal) {
    using lora::viewmodel::ModalId;
    switch (modal) {
        case ModalId::None: return "none";
        case ModalId::Status: return "status";
        case ModalId::Error: return "error";
        case ModalId::Discard: return "discard";
        case ModalId::Exit: return "exit";
        case ModalId::DeleteData: return "delete-data";
        case ModalId::Recovery: return "recovery";
    }
    return "unknown";
}

std::string focus_probe(const lora::viewmodel::ViewSnapshot& snapshot) {
    using lora::viewmodel::ModalId;
    using lora::viewmodel::ScreenId;
    if (snapshot.modal.id == ModalId::Discard ||
        snapshot.modal.id == ModalId::Exit ||
        snapshot.modal.id == ModalId::DeleteData ||
        snapshot.modal.id == ModalId::Recovery) {
        return snapshot.modal.confirm_selected ? "modal.confirm" : "modal.cancel";
    }
    if (snapshot.modal.id == ModalId::Status ||
        snapshot.modal.id == ModalId::Error) {
        return "modal.dismiss";
    }

    switch (snapshot.screen) {
        case ScreenId::Menu: {
            const auto* menu =
                std::get_if<lora::viewmodel::MenuSnapshot>(
                    &snapshot.page);
            return menu &&
                           menu->selected ==
                               lora::viewmodel::MenuItem::Settings
                ? "menu.settings"
                : "menu.talk";
        }
        case ScreenId::Timeline: {
            const auto* timeline =
                std::get_if<lora::viewmodel::TimelineSnapshot>(&snapshot.page);
            return timeline && timeline->rows.empty()
                ? "timeline.empty"
                : "timeline.row";
        }
        case ScreenId::Detail: return "detail.body";
        case ScreenId::Compose: return "compose.body";
        case ScreenId::Mentions: {
            const auto* mentions =
                std::get_if<lora::viewmodel::MentionsSnapshot>(&snapshot.page);
            return mentions && mentions->options.empty()
                ? "mentions.empty"
                : "mentions.option";
        }
        case ScreenId::Settings: {
            const auto* settings =
                std::get_if<lora::viewmodel::SettingsSnapshot>(
                    &snapshot.page);
            return settings &&
                           settings->selected ==
                               lora::viewmodel::SettingsItem::SkipTitle
                ? "settings.skip-title"
                : "settings.language";
        }
    }
    return "unknown";
}

lora::viewmodel::Locale view_locale(
    lora::persistence::StoredLocale locale) noexcept {
    using lora::persistence::StoredLocale;
    switch (locale) {
        case StoredLocale::English:
            return lora::viewmodel::Locale::English;
        case StoredLocale::Japanese:
            return lora::viewmodel::Locale::Japanese;
        case StoredLocale::SimplifiedChinese:
            return lora::viewmodel::Locale::SimplifiedChinese;
    }
    return lora::viewmodel::Locale::English;
}

lora::persistence::StoredLocale stored_locale(
    lora::viewmodel::Locale locale) noexcept {
    using lora::viewmodel::Locale;
    switch (locale) {
        case Locale::English:
            return lora::persistence::StoredLocale::English;
        case Locale::Japanese:
            return lora::persistence::StoredLocale::Japanese;
        case Locale::SimplifiedChinese:
            return lora::persistence::StoredLocale::SimplifiedChinese;
        case Locale::Count:
            break;
    }
    return lora::persistence::StoredLocale::English;
}

class UiSettingsCommitAdapter final
    : public lora::viewmodel::IUiSettingsCommit {
public:
    explicit UiSettingsCommitAdapter(PersistentSession& session)
        : session_(session) {}

    bool persist_locale(
        lora::viewmodel::Locale locale) noexcept override {
        return session_.persist_locale(stored_locale(locale));
    }

    bool persist_skip_title(bool skip_title) noexcept override {
        return session_.persist_skip_title(skip_title);
    }

private:
    PersistentSession& session_;
};

bool demo_seed_requested() noexcept {
    const char* value = std::getenv("LORA_MESSENGER_SEED_DEMO");
    return value != nullptr && std::string_view(value) == "1";
}

lora::ports::RadioTick monotonic_milliseconds() noexcept {
    const auto elapsed =
        std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
    return milliseconds < 0
        ? 0U
        : static_cast<lora::ports::RadioTick>(milliseconds);
}

#if !USE_DESKTOP
bool antenna_confirmed() noexcept {
    const char* value =
        std::getenv("LORA_MESSENGER_ANTENNA_ATTACHED");
    return value && std::string_view(value) == "1";
}

bool wifi_broadcast_enabled() noexcept {
    const char* value =
        std::getenv("LORA_MESSENGER_WIFI_BROADCAST");
    return value && std::string_view(value) == "1";
}

void apply_device_path(
    const char* variable, std::string& destination) {
    const char* value = std::getenv(variable);
    if (value && value[0] != '\0') {
        destination = value;
    }
}
#endif

bool seed_demo_history(PersistentSession& session) {
    if (!demo_seed_requested() || !session.ready() ||
        !session.state().timeline().empty()) {
        return true;
    }
    DemoScenario fixture;
    if (!fixture.ready()) {
        return false;
    }
    for (const auto& entry : fixture.state().timeline().entries()) {
        if (!session.state().accept_received(entry.post).ok()) {
            return false;
        }
    }
    return true;
}

std::string newest_state_probe(
    const lora::application::MessengerState& messenger) {
    const auto* entry = messenger.timeline().newest_at(0);
    if (!entry) {
        return "empty";
    }
    if (std::holds_alternative<lora::model::ReceivedOrigin>(
            entry->origin)) {
        return "received";
    }
    const auto* local =
        std::get_if<lora::model::LocalDelivery>(&entry->origin);
    if (!local) {
        return "unknown";
    }
    using lora::model::LocalDeliveryState;
    switch (local->state) {
        case LocalDeliveryState::Queued: return "queued";
        case LocalDeliveryState::Broadcast: return "broadcast";
        case LocalDeliveryState::Failed: return "failed";
        case LocalDeliveryState::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("lora-messenger");

    lv_init();
    RunState state;
    state.resources = init_display();
    if (!state.resources.display) {
        LOG_ERROR("failed to initialize the display");
        if (lv_is_initialized()) {
            lv_deinit();
        }
        logger::Logger::shutdown();
        return 1;
    }
    lv_display_add_event_cb(state.resources.display, display_deleted_cb, LV_EVENT_DELETE, &state);

    {
        AssetManager assets;
        for (const auto& root : assets.roots()) {
            LOG_INFO("asset root: {}", root.string());
        }

        view::apply_lvgl_theme(state.resources.display);
        view::MessengerFonts fonts{
            {assets.load_font("inter-medium.ttf", 16),
             assets.load_font("inter-regular.ttf", 14)},
            {assets.load_font("lora-ui-ja.otf", 16),
             assets.load_font("lora-ui-ja.otf", 14)},
            {assets.load_font("lora-ui-zh-hans.otf", 16),
             assets.load_font("lora-ui-zh-hans.otf", 14)},
        };
        if (!fonts.complete()) {
            LOG_ERROR("one or more required Phase 2 UI fonts could not be loaded");
            state.request_failure();
        }

        PersistentSession session;
        if (!session.ready() && !session.recovery_required()) {
            LOG_ERROR("failed to open the local persistence session");
            state.request_failure();
        }
        if (state.running && !seed_demo_history(session)) {
            LOG_ERROR("failed to create the simulator test fixture");
            state.request_failure();
        }

#if !USE_DESKTOP
        std::optional<
            lora::adapters::network::PosixUdpBroadcastSocket>
            lan_socket;
        std::optional<
            lora::adapters::network::UdpBroadcastTransport>
            lan_transport;
        std::optional<
            lora::adapters::network::LanBroadcastPolicy>
            lan_policy;
        std::optional<
            lora::adapters::radio::LinuxCapLora1262Radio>
            cap_radio;
        std::optional<
            lora::adapters::radio::CapLora1262Transport>
            cap_transport;
        std::optional<
            lora::adapters::radio::Japan920RadioPolicy>
            radio_policy;
        std::optional<RadioRuntime> radio_runtime;
        auto transport_status =
            lora::viewmodel::TransportStatus::Offline;
        if (state.running && session.ready() &&
            wifi_broadcast_enabled()) {
            lora::adapters::network::PosixUdpBroadcastConfig
                lan_config;
            apply_device_path(
                "LORA_MESSENGER_WIFI_INTERFACE",
                lan_config.interface_name);
            lan_socket.emplace(std::move(lan_config));
            if (lan_socket->ready()) {
                lan_transport.emplace(*lan_socket);
                lan_policy.emplace();
                radio_runtime.emplace(
                    *lan_transport, *lan_policy,
                    session.random_source());
                transport_status =
                    lora::viewmodel::TransportStatus::WifiLan;
            }
        }
        if (!radio_runtime && state.running && session.ready() &&
            antenna_confirmed()) {
            lora::adapters::radio::LinuxCapLora1262Config
                radio_config;
            radio_config.antenna_confirmed = true;
            apply_device_path(
                "LORA_MESSENGER_SPI_DEVICE",
                radio_config.spi_device);
            apply_device_path(
                "LORA_MESSENGER_GPIO_CHIP",
                radio_config.gpio_chip);
            apply_device_path(
                "LORA_MESSENGER_I2C_DEVICE",
                radio_config.i2c_device);
            cap_radio.emplace(std::move(radio_config));
            if (cap_radio->ready()) {
                cap_transport.emplace(*cap_radio);
                radio_policy.emplace();
                radio_runtime.emplace(
                    *cap_transport, *radio_policy,
                    session.random_source());
                if (radio_runtime->ready()) {
                    transport_status =
                        lora::viewmodel::TransportStatus::LoRa;
                }
            }
        }
#endif

        UiSettingsCommitAdapter settings_commit(session);
        lora::viewmodel::MessengerViewModel view_model(
            session.state(), view_locale(session.locale()), &settings_commit,
            session.recovery_required(), session.skip_title());
#if !USE_DESKTOP
        bool reported_radio_ready =
            radio_runtime && radio_runtime->ready();
        view_model.set_transport_status(
            reported_radio_ready
                ? transport_status
                : lora::viewmodel::TransportStatus::Offline);
#endif
        view::MessengerScreen messenger_screen(
            fonts, assets.resolve("images/title_logo.png"));
        if (state.running && !messenger_screen.show(view_model.snapshot())) {
            LOG_ERROR("failed to create the initial messenger screen");
            state.request_failure();
        }

        platform::set_key_handler([&state, &session, &view_model,
                                   &messenger_screen](std::uint32_t key) {
            if (!state.running) {
                return;
            }
            const auto action = view_model.handle(semantic_key(key));
            if (action.delete_data_approved) {
                if (session.delete_all_local_data()) {
                    state.request_stop();
                    return;
                }
                view_model.report_storage_failure();
                if (!messenger_screen.render(view_model.snapshot())) {
                    LOG_ERROR("failed to render the storage error");
                    state.request_failure();
                }
                return;
            }
            if (action.render_required && !messenger_screen.render(view_model.snapshot())) {
                LOG_ERROR("failed to render the messenger screen");
                state.request_failure();
                return;
            }
            if (action.exit_approved) {
                state.request_stop();
            }
        });
        platform::start_app_script([&session, &view_model](std::string_view field)
                                       -> std::optional<std::string> {
            if (field == "screen") {
                return screen_probe(view_model.screen());
            }
            if (field == "modal") {
                return modal_probe(view_model.modal());
            }
            if (field == "locale") {
                if (view_model.locale() ==
                    lora::viewmodel::Locale::SimplifiedChinese) {
                    return std::string{"zh-hans"};
                }
                return std::string{lora::viewmodel::locale_code(view_model.locale())};
            }
            if (field == "focus") {
                return focus_probe(view_model.snapshot());
            }
            if (field == "status") {
                return view_model.modal() == lora::viewmodel::ModalId::Status
                           ? std::optional<std::string>{"queued"}
                           : std::optional<std::string>{"idle"};
            }
            if (field == "count") {
                return std::to_string(session.state().timeline().size());
            }
            if (field == "persistence") {
                return session.ready()
                    ? std::optional<std::string>{"ready"}
                    : std::optional<std::string>{"recovery"};
            }
            if (field == "newest-state") {
                return newest_state_probe(session.state());
            }
            return std::nullopt;
        });

        LOG_INFO("application started at {}x{}; radio adapter {}",
                 lv_display_get_horizontal_resolution(state.resources.display),
                 lv_display_get_vertical_resolution(state.resources.display),
#if USE_DESKTOP
                 "disabled");
#else
                 radio_runtime && radio_runtime->ready()
                     ? (transport_status ==
                                lora::viewmodel::TransportStatus::WifiLan
                            ? "ready (Wi-Fi LAN)"
                            : "ready (JP 920)")
                     : "disabled");
#endif
        while (state.running && lv_is_initialized()) {
            lv_timer_handler();
#if !USE_DESKTOP
            if (radio_runtime) {
                const bool changed = radio_runtime->pump(
                    monotonic_milliseconds(), session);
                const bool radio_ready = radio_runtime->ready();
                if (radio_ready != reported_radio_ready) {
                    reported_radio_ready = radio_ready;
                    view_model.set_transport_status(
                        radio_ready
                            ? transport_status
                            : lora::viewmodel::TransportStatus::Offline);
                    if (!messenger_screen.render(
                            view_model.snapshot())) {
                        LOG_ERROR(
                            "failed to render radio state");
                        state.request_failure();
                    }
                } else if (changed) {
                    view_model.refresh();
                    if (!messenger_screen.render(
                            view_model.snapshot())) {
                        LOG_ERROR(
                            "failed to render radio update");
                        state.request_failure();
                    }
                }
            }
#endif
            if (platform::app_script_failed()) {
                LOG_ERROR("APP_SCRIPT failed; stopping with a non-zero status");
                state.request_failure();
            }
            lv_delay_ms(5);
        }

        if (platform::app_script_pending()) {
            LOG_ERROR("APP_SCRIPT stopped before completing all actions");
            LOG_ERROR("APP_SCRIPT failed; stopping with a non-zero status");
            state.request_failure();
        }
        platform::stop_app_script();
        platform::set_key_handler({});
#if !USE_DESKTOP
        if (radio_runtime) {
            radio_runtime->stop();
        }
#endif
        messenger_screen.shutdown();
        session.shutdown();
    }

    shutdown_display(state.resources);
    LOG_INFO("common UI teardown complete");
    logger::Logger::shutdown();
    return state.exit_code;
}

} // namespace app
