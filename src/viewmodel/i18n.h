/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "application/messenger_state.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace lora::viewmodel {

enum class Locale : std::size_t {
    English,
    Japanese,
    SimplifiedChinese,
    Count,
};

enum class StringId : std::size_t {
    AppTitle,
    TimelineTitle,
    DetailTitle,
    ComposeTitle,
    MentionsTitle,
    SettingsTitle,
    TimelineEmpty,
    NoPeers,
    SettingsUserId,
    SettingsLanguage,
    SettingsSavedLocally,
    RadioDisabled,
    RadioReady,
    StatusTitle,
    ErrorTitle,
    StatusQueued,
    StatusQueuedForRadio,
    StatusPostUnavailable,
    ConfirmDiscardTitle,
    ConfirmDiscardMessage,
    ConfirmExitTitle,
    ConfirmExitMessage,
    ButtonCancel,
    ButtonDiscard,
    ButtonExit,
    BadgeReceived,
    BadgeQueued,
    BadgeBroadcast,
    BadgeFailed,
    BadgeUnknown,
    ReplyOriginalUnavailable,
    MentionLimitReached,
    LocalDemo,
    HeaderDemoExit,
    HeaderRadioExit,
    RadioNoDelivery,
    RadioReadyNoDelivery,
    BytesRemaining,
    ReplyContext,
    MentionedYou,
    SettingsIdentity,
    FooterTimeline,
    FooterDetail,
    FooterCompose,
    FooterMentions,
    FooterSettings,
    ConfirmDeleteTitle,
    ConfirmDeleteMessage,
    RecoveryTitle,
    RecoveryMessage,
    ButtonDelete,
    ErrorPersistenceUnavailable,
    ErrorNone,
    ErrorNotInitialized,
    ErrorAlreadyInitialized,
    ErrorInvalidUserId,
    ErrorInvalidBody,
    ErrorTooManyMentions,
    ErrorDuplicateMention,
    ErrorReplyParentUnavailable,
    ErrorQueueFull,
    ErrorTimelineFull,
    ErrorSequenceExhausted,
    ErrorOrderExhausted,
    ErrorRandomUnavailable,
    ErrorMessageIdCollision,
    ErrorInvalidPost,
    ErrorDuplicatePost,
    ErrorConflictingPost,
    ErrorMessageNotFound,
    ErrorNotLocalPost,
    ErrorInvalidTransition,
    ErrorInvalidCharacter,
    UnknownError,
    LanReady,
    StatusQueuedForLan,
    HeaderLanExit,
    LanReadyNoDelivery,
    MenuTalk,
    MenuSettings,
    SettingsSkipTitle,
    ToggleOn,
    ToggleOff,
    FooterMenu,
    Count,
};

inline constexpr std::size_t kLocaleCount =
    static_cast<std::size_t>(Locale::Count);
inline constexpr std::size_t kStringIdCount =
    static_cast<std::size_t>(StringId::Count);

inline constexpr std::array<Locale, kLocaleCount> kSupportedLocales{
    Locale::English,
    Locale::Japanese,
    Locale::SimplifiedChinese,
};

std::string_view locale_code(Locale locale) noexcept;
std::string_view locale_display_name(Locale locale) noexcept;
std::string_view translate(Locale locale, StringId id) noexcept;
bool translations_complete() noexcept;
StringId command_error_string_id(application::CommandError error) noexcept;

} // namespace lora::viewmodel
