/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/i18n.h"

#include "core/text.h"
#include "test_support.h"

#include <array>
#include <cstddef>

int main() {
    using lora::application::CommandError;
    using lora::viewmodel::Locale;
    using lora::viewmodel::StringId;
    lora::test::Runner runner;

    runner.run("every locale has non-empty valid UTF-8 for every key", [&] {
        CHECK(lora::viewmodel::translations_complete());
        for (const auto locale : lora::viewmodel::kSupportedLocales) {
            for (std::size_t index = 0;
                 index < lora::viewmodel::kStringIdCount; ++index) {
                const auto value = lora::viewmodel::translate(
                    locale, static_cast<StringId>(index));
                CHECK(!value.empty());
                CHECK(lora::core::is_valid_utf8(value));
            }
        }
    });

    runner.run("locale metadata and required phase two labels are complete", [&] {
        CHECK_EQ(lora::viewmodel::locale_code(Locale::English), "en");
        CHECK_EQ(lora::viewmodel::locale_code(Locale::Japanese), "ja");
        CHECK_EQ(lora::viewmodel::locale_code(Locale::SimplifiedChinese),
                 "zh-Hans");
        CHECK_EQ(lora::viewmodel::translate(Locale::English,
                                            StringId::LocalDemo),
                 "LOCAL");
        CHECK_EQ(lora::viewmodel::translate(Locale::Japanese,
                                            StringId::LocalDemo),
                 "ローカル");
        CHECK_EQ(lora::viewmodel::translate(Locale::SimplifiedChinese,
                                            StringId::LocalDemo),
                 "本地");
        for (const auto id : {StringId::BytesRemaining,
                              StringId::ReplyContext,
                              StringId::MentionedYou,
                              StringId::SettingsIdentity}) {
            for (const auto locale : lora::viewmodel::kSupportedLocales) {
                CHECK(!lora::viewmodel::translate(locale, id).empty());
            }
        }
    });

    runner.run("invalid locale and key fall back to safe English", [&] {
        const auto invalid_locale = static_cast<Locale>(999);
        const auto invalid_key = static_cast<StringId>(999);
        CHECK_EQ(lora::viewmodel::locale_code(invalid_locale), "en");
        CHECK_EQ(lora::viewmodel::locale_display_name(invalid_locale), "English");
        CHECK_EQ(lora::viewmodel::translate(invalid_locale,
                                            StringId::TimelineTitle),
                 "Timeline");
        CHECK_EQ(lora::viewmodel::translate(Locale::Japanese, invalid_key),
                 "An unknown error occurred.");
    });

    runner.run("every application command error has a localized key", [&] {
        constexpr std::array errors{
            CommandError::None,
            CommandError::NotInitialized,
            CommandError::AlreadyInitialized,
            CommandError::InvalidUserId,
            CommandError::InvalidBody,
            CommandError::TooManyMentions,
            CommandError::DuplicateMention,
            CommandError::ReplyParentUnavailable,
            CommandError::QueueFull,
            CommandError::TimelineFull,
            CommandError::SequenceExhausted,
            CommandError::OrderExhausted,
            CommandError::RandomUnavailable,
            CommandError::MessageIdCollision,
            CommandError::InvalidPost,
            CommandError::DuplicatePost,
            CommandError::ConflictingPost,
            CommandError::MessageNotFound,
            CommandError::NotLocalPost,
            CommandError::InvalidTransition,
        };
        for (const auto error : errors) {
            const auto key = lora::viewmodel::command_error_string_id(error);
            CHECK_NE(key, StringId::UnknownError);
            for (const auto locale : lora::viewmodel::kSupportedLocales) {
                CHECK(!lora::viewmodel::translate(locale, key).empty());
            }
        }
        CHECK_EQ(lora::viewmodel::command_error_string_id(
                     static_cast<CommandError>(999)),
                 StringId::UnknownError);
    });

    return runner.finish();
}
