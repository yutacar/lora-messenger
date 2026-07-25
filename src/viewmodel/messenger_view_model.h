/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "application/messenger_state.h"
#include "viewmodel/i18n.h"
#include "viewmodel/text_editor.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lora::viewmodel {

inline constexpr std::size_t kVisibleTimelineRows = 2;
inline constexpr std::size_t kVisibleMentionRows = 3;
inline constexpr std::size_t kTimelinePreviewBytes = 72;
inline constexpr std::size_t kDetailLineColumns = 26;
inline constexpr std::size_t kVisibleDetailLines = 2;

enum class ScreenId {
    Menu,
    Timeline,
    Detail,
    Compose,
    Mentions,
    Settings,
};

enum class ModalId {
    None,
    Status,
    Error,
    Discard,
    Exit,
    DeleteData,
    Recovery,
};

enum class UiKey {
    Character,
    Up,
    Down,
    Left,
    Right,
    Enter,
    Escape,
    Home,
    Backspace,
    Delete,
    Tab,
};

struct KeyEvent {
    UiKey key{UiKey::Character};
    char32_t character{0};
};

struct UiAction {
    bool render_required{false};
    bool exit_approved{false};
    bool delete_data_approved{false};
};

enum class DeliveryBadge {
    Received,
    Queued,
    Broadcast,
    Failed,
    Unknown,
};

enum class ReplyAvailability {
    NotReply,
    Available,
    Unavailable,
};

enum class MenuItem {
    Talk,
    Settings,
};

enum class SettingsItem {
    Language,
    SkipTitle,
};

enum class TransportStatus {
    Offline,
    LoRa,
    WifiLan,
};

struct MenuSnapshot {
    MenuItem selected{MenuItem::Talk};
};

struct TimelineRowSnapshot {
    core::MessageId message_id;
    std::string sender;
    std::string sender_suffix;
    std::string body_preview;
    DeliveryBadge delivery;
    bool show_sender_suffix;
    bool is_reply;
    bool mentions_me;
    bool selected;
};

struct TimelineSnapshot {
    std::vector<TimelineRowSnapshot> rows;
    std::size_t total_rows{0};
    std::optional<core::MessageId> selected_message_id;
};

struct MentionLabelSnapshot {
    core::InstallId install_id;
    std::string label;
};

struct DetailSnapshot {
    std::optional<core::MessageId> message_id;
    std::string sender;
    std::string sender_suffix;
    std::string body;
    std::string visible_body;
    DeliveryBadge delivery{DeliveryBadge::Unknown};
    ReplyAvailability reply{ReplyAvailability::NotReply};
    std::string reply_preview;
    std::vector<MentionLabelSnapshot> mentions;
    bool mentions_me{false};
    std::size_t scroll_line{0};
    std::size_t maximum_scroll_line{0};
};

struct ComposeSnapshot {
    std::string body;
    std::size_t cursor_byte_offset{0};
    std::size_t remaining_bytes{0};
    std::optional<core::MessageId> reply_to;
    std::vector<MentionLabelSnapshot> mentions;
    bool dirty{false};
};

struct MentionOptionSnapshot {
    core::InstallId install_id;
    std::string label;
    bool selected;
    bool cursor;
};

struct MentionsSnapshot {
    std::vector<MentionOptionSnapshot> options;
    std::size_t total_options{0};
    std::size_t selected_count{0};
};

struct SettingsSnapshot {
    std::string user_id;
    std::string install_suffix;
    std::string locale_name;
    std::string session_notice;
    std::string radio_status;
    bool skip_title{false};
    SettingsItem selected{SettingsItem::Language};
};

using PageSnapshot = std::variant<MenuSnapshot, TimelineSnapshot, DetailSnapshot,
                                  ComposeSnapshot, MentionsSnapshot,
                                  SettingsSnapshot>;

struct ModalSnapshot {
    ModalId id{ModalId::None};
    std::string title;
    std::string message;
    std::string cancel_label;
    std::string confirm_label;
    bool confirm_selected{false};
};

struct ViewSnapshot {
    ScreenId screen{ScreenId::Timeline};
    Locale locale{Locale::English};
    bool radio_ready{false};
    TransportStatus transport_status{TransportStatus::Offline};
    std::string title;
    std::string footer;
    PageSnapshot page{TimelineSnapshot{}};
    ModalSnapshot modal;
};

class IUiSettingsCommit {
public:
    virtual ~IUiSettingsCommit() = default;
    virtual bool persist_locale(Locale locale) noexcept = 0;
    virtual bool persist_skip_title(bool skip_title) noexcept = 0;
};

class MessengerViewModel {
public:
    explicit MessengerViewModel(application::MessengerState& messenger,
                                Locale locale = Locale::English,
                                IUiSettingsCommit* settings_commit = nullptr,
                                bool recovery_required = false,
                                bool skip_title = true);

    UiAction handle(KeyEvent event);
    ViewSnapshot snapshot();
    void refresh();

    ScreenId screen() const noexcept;
    ModalId modal() const noexcept;
    Locale locale() const noexcept;
    void set_radio_ready(bool ready) noexcept;
    void set_transport_status(TransportStatus status) noexcept;
    std::optional<core::MessageId> selected_message_id() const noexcept;
    void report_storage_failure();

private:
    struct Draft {
        Draft();

        TextEditor body;
        std::vector<core::InstallId> mentions;
        std::optional<core::MessageId> reply_to;

        bool dirty() const noexcept;
        void reset();
    };

    struct ActiveModal {
        ModalId id{ModalId::None};
        StringId message{StringId::UnknownError};
        bool confirm_selected{false};
    };

    void reconcile_model();
    void ensure_timeline_selection();
    void refresh_peers();
    void open_detail();
    void open_compose(std::optional<core::MessageId> reply_to,
                      std::optional<core::InstallId> initial_mention,
                      ScreenId return_screen);
    void close_compose();
    void show_status(StringId message);
    void show_error(StringId message);
    void show_command_error(application::CommandError error);
    void show_exit_confirmation();
    void show_delete_confirmation();

    UiAction handle_modal(KeyEvent event);
    UiAction handle_global_shortcut(KeyEvent event);
    UiAction handle_menu(KeyEvent event);
    UiAction handle_timeline(KeyEvent event);
    UiAction handle_detail(KeyEvent event);
    UiAction handle_compose(KeyEvent event);
    UiAction handle_mentions(KeyEvent event);
    UiAction handle_settings(KeyEvent event);

    MenuSnapshot menu_snapshot() const;
    TimelineSnapshot timeline_snapshot() const;
    DetailSnapshot detail_snapshot() const;
    ComposeSnapshot compose_snapshot() const;
    MentionsSnapshot mentions_snapshot();
    SettingsSnapshot settings_snapshot() const;
    ModalSnapshot modal_snapshot() const;

    const model::TimelineEntry* selected_entry() const noexcept;
    const model::TimelineEntry* detail_entry() const noexcept;
    std::optional<std::size_t> newest_index(
        const core::MessageId& message_id) const noexcept;
    std::string peer_label(const core::InstallId& install_id) const;

    application::MessengerState& messenger_;
    Locale locale_;
    IUiSettingsCommit* settings_commit_;
    bool radio_ready_{false};
    TransportStatus transport_status_{TransportStatus::Offline};
    ScreenId screen_{ScreenId::Timeline};
    ScreenId compose_return_screen_{ScreenId::Timeline};
    ScreenId settings_return_screen_{ScreenId::Timeline};
    ActiveModal active_modal_;
    std::optional<core::MessageId> selected_message_id_;
    std::optional<core::MessageId> detail_message_id_;
    std::size_t detail_scroll_line_{0};
    Draft draft_;
    std::vector<core::InstallId> peers_;
    std::size_t peer_cursor_{0};
    MenuItem menu_item_{MenuItem::Talk};
    SettingsItem settings_item_{SettingsItem::Language};
    bool skip_title_{false};
};

} // namespace lora::viewmodel
