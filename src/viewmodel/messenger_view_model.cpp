/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/messenger_view_model.h"

#include "core/limits.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace lora::viewmodel {
namespace {

bool is_shortcut(const KeyEvent& event, char lower_case) noexcept {
    if (event.key != UiKey::Character) {
        return false;
    }
    return event.character == static_cast<char32_t>(lower_case) ||
           event.character == static_cast<char32_t>(lower_case - 'a' + 'A');
}

bool is_editable_scalar(char32_t value) noexcept {
    const auto codepoint = static_cast<std::uint32_t>(value);
    if (codepoint == 0 || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return false;
    }
    return codepoint >= 0x20U && !(codepoint >= 0x7fU && codepoint <= 0x9fU);
}

bool is_continuation(unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

std::string truncated_utf8(std::string_view value, std::size_t maximum_bytes) {
    if (value.size() <= maximum_bytes) {
        return std::string(value);
    }
    if (maximum_bytes <= 3) {
        return std::string(maximum_bytes, '.');
    }
    std::size_t prefix = maximum_bytes - 3;
    while (prefix > 0 &&
           is_continuation(static_cast<unsigned char>(value[prefix]))) {
        --prefix;
    }
    return std::string(value.substr(0, prefix)) + "...";
}

std::size_t utf8_scalar_length(unsigned char lead) noexcept {
    if (lead <= 0x7fU) return 1;
    if (lead <= 0xdfU) return 2;
    if (lead <= 0xefU) return 3;
    return 4;
}

std::size_t detail_columns(std::string_view line) noexcept {
    std::size_t columns = 0;
    for (std::size_t offset = 0; offset < line.size();) {
        const auto lead = static_cast<unsigned char>(line[offset]);
        columns += 2U;
        offset += utf8_scalar_length(lead);
    }
    return columns;
}

struct WrappedDetailBody {
    std::vector<std::string> lines;
    std::size_t maximum_scroll_line{0};
};

WrappedDetailBody wrap_detail_body(std::string_view body) {
    WrappedDetailBody wrapped;
    wrapped.lines.emplace_back();
    std::size_t columns = 0;
    std::size_t last_space = std::string::npos;
    for (std::size_t offset = 0; offset < body.size();) {
        const auto lead = static_cast<unsigned char>(body[offset]);
        if (lead == static_cast<unsigned char>('\n')) {
            wrapped.lines.emplace_back();
            columns = 0;
            last_space = std::string::npos;
            ++offset;
            continue;
        }

        const auto scalar_bytes = utf8_scalar_length(lead);
        // Every scalar consumes two conservative cells. With the bundled
        // 16 px fonts this caps a row at 13 glyphs, so LVGL does not wrap a
        // logical row again for wide Latin or CJK text.
        constexpr std::size_t scalar_columns = 2U;
        if (columns > 0 && columns + scalar_columns > kDetailLineColumns) {
            auto& line = wrapped.lines.back();
            if (last_space != std::string::npos) {
                auto carry = line.substr(last_space + 1U);
                line.erase(last_space);
                wrapped.lines.push_back(std::move(carry));
                columns = detail_columns(wrapped.lines.back());
                last_space = wrapped.lines.back().find_last_of(' ');
            } else {
                wrapped.lines.emplace_back();
                columns = 0;
                last_space = std::string::npos;
            }
            continue;
        }
        if (lead == static_cast<unsigned char>(' ')) {
            last_space = wrapped.lines.back().size();
        }
        wrapped.lines.back().append(body.substr(offset, scalar_bytes));
        columns += scalar_columns;
        offset += scalar_bytes;
    }
    wrapped.maximum_scroll_line =
        wrapped.lines.size() > kVisibleDetailLines
            ? wrapped.lines.size() - kVisibleDetailLines
            : 0U;
    return wrapped;
}

std::string detail_window(const WrappedDetailBody& wrapped,
                          std::size_t scroll_line) {
    if (wrapped.lines.empty()) {
        return {};
    }
    const auto first = std::min(scroll_line, wrapped.maximum_scroll_line);
    const auto last = std::min(wrapped.lines.size(), first + kVisibleDetailLines);
    std::string result;
    for (std::size_t index = first; index < last; ++index) {
        if (index != first) {
            result.push_back('\n');
        }
        result += wrapped.lines[index];
    }
    return result;
}

std::string id_suffix(const core::InstallId& install_id) {
    const auto value = install_id.to_string();
    constexpr std::size_t kSuffixLength = 4;
    return value.size() <= kSuffixLength
        ? value
        : value.substr(value.size() - kSuffixLength);
}

DeliveryBadge delivery_badge(const model::TimelineEntry& entry) noexcept {
    if (std::holds_alternative<model::ReceivedOrigin>(entry.origin)) {
        return DeliveryBadge::Received;
    }
    const auto* local = std::get_if<model::LocalDelivery>(&entry.origin);
    if (!local) {
        return DeliveryBadge::Unknown;
    }
    switch (local->state) {
        case model::LocalDeliveryState::Queued: return DeliveryBadge::Queued;
        case model::LocalDeliveryState::Broadcast: return DeliveryBadge::Broadcast;
        case model::LocalDeliveryState::Failed: return DeliveryBadge::Failed;
        case model::LocalDeliveryState::Unknown: return DeliveryBadge::Unknown;
    }
    return DeliveryBadge::Unknown;
}

StringId screen_title(ScreenId screen) noexcept {
    switch (screen) {
        case ScreenId::Menu: return StringId::AppTitle;
        case ScreenId::Timeline: return StringId::TimelineTitle;
        case ScreenId::Detail: return StringId::DetailTitle;
        case ScreenId::Compose: return StringId::ComposeTitle;
        case ScreenId::Mentions: return StringId::MentionsTitle;
        case ScreenId::Settings: return StringId::SettingsTitle;
    }
    return StringId::AppTitle;
}

StringId screen_footer(ScreenId screen) noexcept {
    switch (screen) {
        case ScreenId::Menu: return StringId::FooterMenu;
        case ScreenId::Timeline: return StringId::FooterTimeline;
        case ScreenId::Detail: return StringId::FooterDetail;
        case ScreenId::Compose: return StringId::FooterCompose;
        case ScreenId::Mentions: return StringId::FooterMentions;
        case ScreenId::Settings: return StringId::FooterSettings;
    }
    return StringId::FooterTimeline;
}

} // namespace

MessengerViewModel::Draft::Draft() : body(core::kMaxPostBodyBytes) {}

bool MessengerViewModel::Draft::dirty() const noexcept {
    return !body.empty() || !mentions.empty();
}

void MessengerViewModel::Draft::reset() {
    body.clear();
    mentions.clear();
    reply_to.reset();
}

MessengerViewModel::MessengerViewModel(application::MessengerState& messenger,
                                       Locale locale,
                                       IUiSettingsCommit* settings_commit,
                                       bool recovery_required,
                                       bool skip_title)
    : messenger_(messenger),
      locale_(static_cast<std::size_t>(locale) < kLocaleCount
                  ? locale
                  : Locale::English),
      settings_commit_(settings_commit),
      screen_(skip_title ? ScreenId::Timeline : ScreenId::Menu),
      settings_return_screen_(skip_title ? ScreenId::Timeline
                                         : ScreenId::Menu),
      skip_title_(skip_title) {
    ensure_timeline_selection();
    if (recovery_required) {
        active_modal_ = {ModalId::Recovery, StringId::RecoveryMessage, false};
    }
}

UiAction MessengerViewModel::handle(KeyEvent event) {
    reconcile_model();
    if (active_modal_.id != ModalId::None) {
        return handle_modal(event);
    }
    if (event.key == UiKey::Home) {
        show_exit_confirmation();
        return {true, false};
    }
    const auto global_shortcut = handle_global_shortcut(event);
    if (global_shortcut.render_required || global_shortcut.exit_approved ||
        global_shortcut.delete_data_approved) {
        return global_shortcut;
    }

    switch (screen_) {
        case ScreenId::Menu: return handle_menu(event);
        case ScreenId::Timeline: return handle_timeline(event);
        case ScreenId::Detail: return handle_detail(event);
        case ScreenId::Compose: return handle_compose(event);
        case ScreenId::Mentions: return handle_mentions(event);
        case ScreenId::Settings: return handle_settings(event);
    }
    return {};
}

ViewSnapshot MessengerViewModel::snapshot() {
    reconcile_model();

    ViewSnapshot result;
    result.screen = screen_;
    result.locale = locale_;
    result.radio_ready = radio_ready_;
    result.transport_status = transport_status_;
    result.title = std::string(translate(locale_, screen_title(screen_)));
    result.footer = std::string(translate(locale_, screen_footer(screen_)));
    switch (screen_) {
        case ScreenId::Menu: result.page = menu_snapshot(); break;
        case ScreenId::Timeline: result.page = timeline_snapshot(); break;
        case ScreenId::Detail: result.page = detail_snapshot(); break;
        case ScreenId::Compose: result.page = compose_snapshot(); break;
        case ScreenId::Mentions: result.page = mentions_snapshot(); break;
        case ScreenId::Settings: result.page = settings_snapshot(); break;
    }
    result.modal = modal_snapshot();
    return result;
}

void MessengerViewModel::refresh() {
    reconcile_model();
}

ScreenId MessengerViewModel::screen() const noexcept { return screen_; }
ModalId MessengerViewModel::modal() const noexcept { return active_modal_.id; }
Locale MessengerViewModel::locale() const noexcept { return locale_; }
void MessengerViewModel::set_radio_ready(bool ready) noexcept {
    radio_ready_ = ready;
    transport_status_ =
        ready ? TransportStatus::LoRa : TransportStatus::Offline;
}
void MessengerViewModel::set_transport_status(
    TransportStatus status) noexcept {
    transport_status_ = status;
    radio_ready_ = status != TransportStatus::Offline;
}
std::optional<core::MessageId> MessengerViewModel::selected_message_id() const noexcept {
    return selected_message_id_;
}

void MessengerViewModel::report_storage_failure() {
    show_error(StringId::ErrorPersistenceUnavailable);
}

void MessengerViewModel::reconcile_model() {
    ensure_timeline_selection();

    if (screen_ == ScreenId::Detail &&
        (!detail_message_id_ || !messenger_.timeline().find(*detail_message_id_))) {
        detail_message_id_.reset();
        detail_scroll_line_ = 0;
        screen_ = ScreenId::Timeline;
        if (active_modal_.id == ModalId::None) {
            show_status(StringId::StatusPostUnavailable);
        }
    }
    if (screen_ == ScreenId::Mentions) {
        refresh_peers();
    }
}

void MessengerViewModel::ensure_timeline_selection() {
    if (selected_message_id_ &&
        messenger_.timeline().find(*selected_message_id_)) {
        return;
    }
    const auto* newest = messenger_.timeline().newest_at(0);
    selected_message_id_ = newest
        ? std::optional<core::MessageId>{newest->post.message_id()}
        : std::nullopt;
}

void MessengerViewModel::refresh_peers() {
    std::optional<core::InstallId> old_cursor;
    if (peer_cursor_ < peers_.size()) {
        old_cursor = peers_[peer_cursor_];
    }

    peers_.clear();
    const auto& identity = messenger_.identity();
    for (std::size_t index = 0; index < messenger_.timeline().size(); ++index) {
        const auto* entry = messenger_.timeline().newest_at(index);
        if (!entry) {
            continue;
        }
        const auto sender = entry->post.sender_id();
        if (identity && sender == identity->install_id()) {
            continue;
        }
        if (std::find(peers_.begin(), peers_.end(), sender) == peers_.end()) {
            peers_.push_back(sender);
        }
    }
    for (const auto& selected : draft_.mentions) {
        if ((!identity || selected != identity->install_id()) &&
            std::find(peers_.begin(), peers_.end(), selected) == peers_.end()) {
            peers_.push_back(selected);
        }
    }

    peer_cursor_ = 0;
    if (old_cursor) {
        const auto iterator = std::find(peers_.begin(), peers_.end(), *old_cursor);
        if (iterator != peers_.end()) {
            peer_cursor_ = static_cast<std::size_t>(iterator - peers_.begin());
        }
    }
    if (!peers_.empty() && peer_cursor_ >= peers_.size()) {
        peer_cursor_ = peers_.size() - 1;
    }
}

void MessengerViewModel::open_detail() {
    if (!selected_entry() || !selected_message_id_) {
        return;
    }
    detail_message_id_ = selected_message_id_;
    detail_scroll_line_ = 0;
    screen_ = ScreenId::Detail;
}

void MessengerViewModel::open_compose(
    std::optional<core::MessageId> reply_to,
    std::optional<core::InstallId> initial_mention,
    ScreenId return_screen) {
    draft_.reset();
    draft_.reply_to = std::move(reply_to);
    if (initial_mention) {
        const auto& identity = messenger_.identity();
        if (!identity || *initial_mention != identity->install_id()) {
            draft_.mentions.push_back(*initial_mention);
        }
    }
    compose_return_screen_ = return_screen;
    screen_ = ScreenId::Compose;
}

void MessengerViewModel::close_compose() {
    draft_.reset();
    if (compose_return_screen_ == ScreenId::Detail && detail_message_id_ &&
        messenger_.timeline().find(*detail_message_id_)) {
        screen_ = ScreenId::Detail;
    } else if (compose_return_screen_ == ScreenId::Settings) {
        screen_ = ScreenId::Settings;
    } else {
        screen_ = ScreenId::Timeline;
    }
}

void MessengerViewModel::show_status(StringId message) {
    active_modal_ = {ModalId::Status, message, false};
}

void MessengerViewModel::show_error(StringId message) {
    active_modal_ = {ModalId::Error, message, false};
}

void MessengerViewModel::show_command_error(application::CommandError error) {
    show_error(command_error_string_id(error));
}

void MessengerViewModel::show_exit_confirmation() {
    active_modal_ = {ModalId::Exit, StringId::ConfirmExitMessage, false};
}

void MessengerViewModel::show_delete_confirmation() {
    active_modal_ = {
        ModalId::DeleteData, StringId::ConfirmDeleteMessage, false};
}

UiAction MessengerViewModel::handle_modal(KeyEvent event) {
    if (event.key == UiKey::Home &&
        active_modal_.id != ModalId::Exit &&
        active_modal_.id != ModalId::Recovery) {
        show_exit_confirmation();
        return {true, false};
    }

    if (active_modal_.id == ModalId::Status ||
        active_modal_.id == ModalId::Error) {
        if (event.key == UiKey::Escape || event.key == UiKey::Enter) {
            active_modal_ = {};
            return {true, false};
        }
        return {};
    }

    if (active_modal_.id == ModalId::Discard ||
        active_modal_.id == ModalId::Exit ||
        active_modal_.id == ModalId::DeleteData ||
        active_modal_.id == ModalId::Recovery) {
        if (event.key == UiKey::Escape) {
            if (active_modal_.id == ModalId::Recovery) {
                return {false, true, false};
            }
            active_modal_ = {};
            return {true, false};
        }
        if (event.key == UiKey::Left) {
            active_modal_.confirm_selected = false;
            return {true, false};
        }
        if (event.key == UiKey::Right) {
            active_modal_.confirm_selected = true;
            return {true, false};
        }
        if (event.key == UiKey::Enter) {
            if (!active_modal_.confirm_selected) {
                if (active_modal_.id == ModalId::Recovery) {
                    return {false, true, false};
                }
                active_modal_ = {};
                return {true, false};
            }
            const auto confirmed = active_modal_.id;
            active_modal_ = {};
            if (confirmed == ModalId::Discard) {
                close_compose();
                return {true, false};
            }
            if (confirmed == ModalId::DeleteData ||
                confirmed == ModalId::Recovery) {
                return {false, false, true};
            }
            return {false, true};
        }
    }
    return {};
}

UiAction MessengerViewModel::handle_global_shortcut(KeyEvent event) {
    if (screen_ == ScreenId::Compose || screen_ == ScreenId::Mentions) {
        return {};
    }
    if (screen_ == ScreenId::Menu) {
        if (is_shortcut(event, 's')) {
            settings_return_screen_ = ScreenId::Menu;
            screen_ = ScreenId::Settings;
            settings_item_ = SettingsItem::Language;
            return {true, false};
        }
        return {};
    }

    const auto target_id = screen_ == ScreenId::Detail
        ? detail_message_id_
        : selected_message_id_;
    const auto* target = target_id
        ? messenger_.timeline().find(*target_id)
        : nullptr;
    const auto return_screen = screen_;

    if (is_shortcut(event, 'n')) {
        open_compose(std::nullopt, std::nullopt, return_screen);
        return {true, false};
    }
    if (is_shortcut(event, 'r') && target_id) {
        open_compose(target_id, std::nullopt, return_screen);
        return {true, false};
    }
    if (is_shortcut(event, 'm')) {
        open_compose(std::nullopt,
                     target ? std::optional<core::InstallId>{target->post.sender_id()}
                            : std::nullopt,
                     return_screen);
        return {true, false};
    }
    if (is_shortcut(event, 's') && screen_ != ScreenId::Settings) {
        settings_return_screen_ = screen_;
        screen_ = ScreenId::Settings;
        settings_item_ = SettingsItem::Language;
        return {true, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_menu(KeyEvent event) {
    if (event.key == UiKey::Up || event.key == UiKey::Down) {
        menu_item_ = menu_item_ == MenuItem::Talk
            ? MenuItem::Settings
            : MenuItem::Talk;
        return {true, false};
    }
    if (event.key == UiKey::Enter) {
        if (menu_item_ == MenuItem::Talk) {
            screen_ = ScreenId::Timeline;
        } else {
            settings_return_screen_ = ScreenId::Menu;
            settings_item_ = SettingsItem::Language;
            screen_ = ScreenId::Settings;
        }
        return {true, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_timeline(KeyEvent event) {
    if (event.key == UiKey::Escape) {
        screen_ = ScreenId::Menu;
        return {true, false};
    }
    const auto selected_index = selected_message_id_
        ? newest_index(*selected_message_id_)
        : std::nullopt;

    if (event.key == UiKey::Up && selected_index && *selected_index > 0) {
        const auto* entry = messenger_.timeline().newest_at(*selected_index - 1);
        selected_message_id_ = entry->post.message_id();
        return {true, false};
    }
    if (event.key == UiKey::Down && selected_index &&
        *selected_index + 1 < messenger_.timeline().size()) {
        const auto* entry = messenger_.timeline().newest_at(*selected_index + 1);
        selected_message_id_ = entry->post.message_id();
        return {true, false};
    }
    if (event.key == UiKey::Enter) {
        const auto before = screen_;
        open_detail();
        return {screen_ != before, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_detail(KeyEvent event) {
    if (event.key == UiKey::Escape) {
        screen_ = ScreenId::Timeline;
        detail_scroll_line_ = 0;
        return {true, false};
    }
    if (event.key == UiKey::Up && detail_scroll_line_ > 0) {
        --detail_scroll_line_;
        return {true, false};
    }
    const auto* entry = detail_entry();
    const auto maximum_scroll_line = entry
        ? wrap_detail_body(entry->post.body().value()).maximum_scroll_line
        : 0U;
    if (event.key == UiKey::Down &&
        detail_scroll_line_ < maximum_scroll_line) {
        ++detail_scroll_line_;
        return {true, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_compose(KeyEvent event) {
    if (event.key == UiKey::Character) {
        if (!is_editable_scalar(event.character)) {
            show_error(StringId::ErrorInvalidCharacter);
            return {true, false};
        }
        const auto error = draft_.body.insert(event.character);
        if (error != EditorError::None) {
            show_error(error == EditorError::TooLong
                           ? StringId::ErrorInvalidBody
                           : StringId::ErrorInvalidCharacter);
        }
        return {true, false};
    }
    if (event.key == UiKey::Left) {
        return {draft_.body.move_left(), false};
    }
    if (event.key == UiKey::Right) {
        return {draft_.body.move_right(), false};
    }
    if (event.key == UiKey::Backspace) {
        return {draft_.body.backspace(), false};
    }
    if (event.key == UiKey::Delete) {
        return {draft_.body.delete_forward(), false};
    }
    if (event.key == UiKey::Tab) {
        refresh_peers();
        screen_ = ScreenId::Mentions;
        return {true, false};
    }
    if (event.key == UiKey::Escape) {
        if (draft_.dirty()) {
            active_modal_ = {ModalId::Discard, StringId::ConfirmDiscardMessage, false};
        } else {
            close_compose();
        }
        return {true, false};
    }
    if (event.key == UiKey::Enter) {
        model::PostDraft post;
        post.body = draft_.body.text();
        post.mentions = draft_.mentions;
        post.reply_to = draft_.reply_to;
        auto result = messenger_.compose(std::move(post));
        if (!result.ok()) {
            show_command_error(result.error);
            return {true, false};
        }
        selected_message_id_ = result.message_id;
        detail_message_id_.reset();
        detail_scroll_line_ = 0;
        draft_.reset();
        screen_ = ScreenId::Timeline;
        ensure_timeline_selection();
        show_status(
            transport_status_ == TransportStatus::WifiLan
                ? StringId::StatusQueuedForLan
                : (radio_ready_ ? StringId::StatusQueuedForRadio
                                : StringId::StatusQueued));
        return {true, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_mentions(KeyEvent event) {
    refresh_peers();
    if (event.key == UiKey::Escape || event.key == UiKey::Tab) {
        screen_ = ScreenId::Compose;
        return {true, false};
    }
    if (event.key == UiKey::Up && peer_cursor_ > 0) {
        --peer_cursor_;
        return {true, false};
    }
    if (event.key == UiKey::Down && peer_cursor_ + 1 < peers_.size()) {
        ++peer_cursor_;
        return {true, false};
    }
    if (event.key == UiKey::Enter && peer_cursor_ < peers_.size()) {
        const auto peer = peers_[peer_cursor_];
        const auto existing = std::find(draft_.mentions.begin(),
                                        draft_.mentions.end(), peer);
        if (existing != draft_.mentions.end()) {
            draft_.mentions.erase(existing);
            return {true, false};
        }
        if (draft_.mentions.size() >= core::kMaxMentions) {
            show_error(StringId::MentionLimitReached);
            return {true, false};
        }
        draft_.mentions.push_back(peer);
        return {true, false};
    }
    return {};
}

UiAction MessengerViewModel::handle_settings(KeyEvent event) {
    if (is_shortcut(event, 'd')) {
        show_delete_confirmation();
        return {true, false};
    }
    if (event.key == UiKey::Escape) {
        if (settings_return_screen_ == ScreenId::Detail && detail_message_id_ &&
            messenger_.timeline().find(*detail_message_id_)) {
            screen_ = ScreenId::Detail;
        } else {
            screen_ = settings_return_screen_ == ScreenId::Settings
                ? ScreenId::Timeline
                : settings_return_screen_;
        }
        return {true, false};
    }

    if (event.key == UiKey::Up || event.key == UiKey::Down) {
        settings_item_ = settings_item_ == SettingsItem::Language
            ? SettingsItem::SkipTitle
            : SettingsItem::Language;
        return {true, false};
    }

    if (settings_item_ == SettingsItem::SkipTitle &&
        (event.key == UiKey::Left || event.key == UiKey::Right ||
         event.key == UiKey::Enter)) {
        const bool candidate = !skip_title_;
        if (settings_commit_ &&
            !settings_commit_->persist_skip_title(candidate)) {
            show_error(StringId::ErrorPersistenceUnavailable);
            return {true, false};
        }
        skip_title_ = candidate;
        return {true, false};
    }

    const auto current = static_cast<std::size_t>(locale_);
    if (settings_item_ == SettingsItem::Language &&
        event.key == UiKey::Left) {
        const auto candidate =
            kSupportedLocales[(current + kLocaleCount - 1) % kLocaleCount];
        if (settings_commit_ &&
            !settings_commit_->persist_locale(candidate)) {
            show_error(StringId::ErrorPersistenceUnavailable);
            return {true, false};
        }
        locale_ = candidate;
        return {true, false};
    }
    if (settings_item_ == SettingsItem::Language &&
        (event.key == UiKey::Right || event.key == UiKey::Enter)) {
        const auto candidate =
            kSupportedLocales[(current + 1) % kLocaleCount];
        if (settings_commit_ &&
            !settings_commit_->persist_locale(candidate)) {
            show_error(StringId::ErrorPersistenceUnavailable);
            return {true, false};
        }
        locale_ = candidate;
        return {true, false};
    }
    return {};
}

MenuSnapshot MessengerViewModel::menu_snapshot() const {
    return {menu_item_};
}

TimelineSnapshot MessengerViewModel::timeline_snapshot() const {
    TimelineSnapshot result;
    result.total_rows = messenger_.timeline().size();
    result.selected_message_id = selected_message_id_;
    if (result.total_rows == 0) {
        return result;
    }

    const auto selected_index = selected_message_id_
        ? newest_index(*selected_message_id_).value_or(0)
        : 0;
    std::size_t first = selected_index;
    if (first + kVisibleTimelineRows > result.total_rows) {
        first = result.total_rows > kVisibleTimelineRows
            ? result.total_rows - kVisibleTimelineRows
            : 0;
    }
    const auto last = std::min(result.total_rows, first + kVisibleTimelineRows);
    result.rows.reserve(last - first);

    const auto& entries = messenger_.timeline().entries();
    const auto& identity = messenger_.identity();
    for (std::size_t index = first; index < last; ++index) {
        const auto* entry = messenger_.timeline().newest_at(index);
        if (!entry) {
            continue;
        }
        bool duplicate_name = false;
        for (const auto& candidate : entries) {
            if (candidate.post.sender_id() != entry->post.sender_id() &&
                candidate.post.sender_user_id().value() ==
                    entry->post.sender_user_id().value()) {
                duplicate_name = true;
                break;
            }
        }
        result.rows.push_back(TimelineRowSnapshot{
            entry->post.message_id(),
            entry->post.sender_user_id().value(),
            id_suffix(entry->post.sender_id()),
            truncated_utf8(entry->post.body().value(), kTimelinePreviewBytes),
            delivery_badge(*entry),
            duplicate_name,
            entry->post.reply_to().has_value(),
            identity && messenger_.timeline().mentions(*entry, identity->install_id()),
            selected_message_id_ &&
                entry->post.message_id() == *selected_message_id_,
        });
    }
    return result;
}

DetailSnapshot MessengerViewModel::detail_snapshot() const {
    DetailSnapshot result;
    const auto* entry = detail_entry();
    if (!entry) {
        return result;
    }

    result.message_id = entry->post.message_id();
    result.sender = entry->post.sender_user_id().value();
    result.sender_suffix = id_suffix(entry->post.sender_id());
    result.body = entry->post.body().value();
    const auto wrapped = wrap_detail_body(result.body);
    result.maximum_scroll_line = wrapped.maximum_scroll_line;
    result.scroll_line = std::min(detail_scroll_line_, result.maximum_scroll_line);
    result.visible_body = detail_window(wrapped, result.scroll_line);
    result.delivery = delivery_badge(*entry);
    const auto& identity = messenger_.identity();
    result.mentions_me = identity &&
        messenger_.timeline().mentions(*entry, identity->install_id());

    switch (messenger_.timeline().reply_state(*entry)) {
        case model::ReplyState::NotReply:
            result.reply = ReplyAvailability::NotReply;
            break;
        case model::ReplyState::ParentAvailable: {
            result.reply = ReplyAvailability::Available;
            const auto* parent = messenger_.timeline().find(*entry->post.reply_to());
            if (parent) {
                result.reply_preview = truncated_utf8(
                    parent->post.body().value(), kTimelinePreviewBytes);
            }
            break;
        }
        case model::ReplyState::ParentUnavailable:
            result.reply = ReplyAvailability::Unavailable;
            result.reply_preview = std::string(
                translate(locale_, StringId::ReplyOriginalUnavailable));
            break;
    }

    result.mentions.reserve(entry->post.mentions().size());
    for (const auto& mention : entry->post.mentions()) {
        result.mentions.push_back({mention, peer_label(mention)});
    }
    return result;
}

ComposeSnapshot MessengerViewModel::compose_snapshot() const {
    ComposeSnapshot result;
    result.body = draft_.body.text();
    result.cursor_byte_offset = draft_.body.cursor_byte_offset();
    result.remaining_bytes = draft_.body.remaining_bytes();
    result.reply_to = draft_.reply_to;
    result.dirty = draft_.dirty();
    result.mentions.reserve(draft_.mentions.size());
    for (const auto& mention : draft_.mentions) {
        result.mentions.push_back({mention, peer_label(mention)});
    }
    return result;
}

MentionsSnapshot MessengerViewModel::mentions_snapshot() {
    refresh_peers();
    MentionsSnapshot result;
    result.total_options = peers_.size();
    result.selected_count = draft_.mentions.size();
    if (peers_.empty()) {
        return result;
    }

    std::size_t first = peer_cursor_;
    if (first + kVisibleMentionRows > peers_.size()) {
        first = peers_.size() > kVisibleMentionRows
            ? peers_.size() - kVisibleMentionRows
            : 0;
    }
    const auto last = std::min(peers_.size(), first + kVisibleMentionRows);
    result.options.reserve(last - first);
    for (std::size_t index = first; index < last; ++index) {
        const auto& peer = peers_[index];
        result.options.push_back({
            peer,
            peer_label(peer),
            std::find(draft_.mentions.begin(), draft_.mentions.end(), peer) !=
                draft_.mentions.end(),
            index == peer_cursor_,
        });
    }
    return result;
}

SettingsSnapshot MessengerViewModel::settings_snapshot() const {
    SettingsSnapshot result;
    if (messenger_.identity()) {
        result.user_id = messenger_.identity()->user_id().value();
        result.install_suffix = id_suffix(messenger_.identity()->install_id());
    }
    result.locale_name = std::string(locale_display_name(locale_));
    result.session_notice = std::string(
        translate(locale_, StringId::SettingsSavedLocally));
    result.radio_status = std::string(
        translate(
            locale_,
            transport_status_ == TransportStatus::WifiLan
                ? StringId::LanReadyNoDelivery
                : (radio_ready_ ? StringId::RadioReadyNoDelivery
                                : StringId::RadioNoDelivery)));
    result.skip_title = skip_title_;
    result.selected = settings_item_;
    return result;
}

ModalSnapshot MessengerViewModel::modal_snapshot() const {
    ModalSnapshot result;
    result.id = active_modal_.id;
    result.confirm_selected = active_modal_.confirm_selected;
    if (active_modal_.id == ModalId::None) {
        return result;
    }
    if (active_modal_.id == ModalId::Status) {
        result.title = std::string(translate(locale_, StringId::StatusTitle));
    } else if (active_modal_.id == ModalId::Error) {
        result.title = std::string(translate(locale_, StringId::ErrorTitle));
    } else if (active_modal_.id == ModalId::Discard) {
        result.title = std::string(
            translate(locale_, StringId::ConfirmDiscardTitle));
        result.cancel_label = std::string(
            translate(locale_, StringId::ButtonCancel));
        result.confirm_label = std::string(
            translate(locale_, StringId::ButtonDiscard));
    } else if (active_modal_.id == ModalId::Exit) {
        result.title = std::string(
            translate(locale_, StringId::ConfirmExitTitle));
        result.cancel_label = std::string(
            translate(locale_, StringId::ButtonCancel));
        result.confirm_label = std::string(
            translate(locale_, StringId::ButtonExit));
    } else if (active_modal_.id == ModalId::DeleteData) {
        result.title = std::string(
            translate(locale_, StringId::ConfirmDeleteTitle));
        result.cancel_label = std::string(
            translate(locale_, StringId::ButtonCancel));
        result.confirm_label = std::string(
            translate(locale_, StringId::ButtonDelete));
    } else if (active_modal_.id == ModalId::Recovery) {
        result.title = std::string(
            translate(locale_, StringId::RecoveryTitle));
        result.cancel_label = std::string(
            translate(locale_, StringId::ButtonExit));
        result.confirm_label = std::string(
            translate(locale_, StringId::ButtonDelete));
    }
    result.message = std::string(translate(locale_, active_modal_.message));
    return result;
}

const model::TimelineEntry* MessengerViewModel::selected_entry() const noexcept {
    return selected_message_id_
        ? messenger_.timeline().find(*selected_message_id_)
        : nullptr;
}

const model::TimelineEntry* MessengerViewModel::detail_entry() const noexcept {
    return detail_message_id_
        ? messenger_.timeline().find(*detail_message_id_)
        : nullptr;
}

std::optional<std::size_t> MessengerViewModel::newest_index(
    const core::MessageId& message_id) const noexcept {
    for (std::size_t index = 0; index < messenger_.timeline().size(); ++index) {
        const auto* entry = messenger_.timeline().newest_at(index);
        if (entry && entry->post.message_id() == message_id) {
            return index;
        }
    }
    return std::nullopt;
}

std::string MessengerViewModel::peer_label(
    const core::InstallId& install_id) const {
    for (std::size_t index = 0; index < messenger_.timeline().size(); ++index) {
        const auto* entry = messenger_.timeline().newest_at(index);
        if (entry && entry->post.sender_id() == install_id) {
            return entry->post.sender_user_id().value() + " #" +
                   id_suffix(install_id);
        }
    }
    return std::string(translate(locale_, StringId::BadgeUnknown)) + " #" +
           id_suffix(install_id);
}

} // namespace lora::viewmodel
