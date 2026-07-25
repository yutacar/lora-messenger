/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/i18n.h"

#include <array>

namespace lora::viewmodel {
namespace {

using TranslationTable = std::array<std::string_view, kStringIdCount>;

constexpr TranslationTable kEnglish{
    "LoRa Messenger",
    "Timeline",
    "Post detail",
    "New post",
    "Mentions",
    "Settings",
    "No messages yet",
    "No people in local history",
    "User ID",
    "Language",
    "Saved locally",
    "Radio disabled",
    "JP LoRa ready",
    "Status",
    "Error",
    "Saved locally as queued. Radio is disabled; delivery is unconfirmed.",
    "Queued for JP LoRa broadcast. Peer delivery is unconfirmed.",
    "That post is no longer in local history.",
    "Discard draft?",
    "The unsent draft will be lost.",
    "Exit LoRa Messenger?",
    "Local settings and history will remain.",
    "Cancel",
    "Discard",
    "Exit",
    "Received",
    "Queued",
    "Broadcast",
    "Failed",
    "Unknown",
    "Original post unavailable",
    "A post can mention at most 4 people.",
    "LOCAL",
    "LOCAL / H Exit",
    "JP LORA / H Exit",
    "Radio disabled / no delivery confirmation",
    "JP LoRa ready / no peer delivery confirmation",
    "Bytes remaining",
    "Reply",
    "Mentioned you",
    "Identity",
    "^v Sel  Ent Open  N New  R Reply  M @  S Set",
    "Up/Down Scroll  R Reply  M @  Esc Back",
    "Type  Enter Send  Tab @  Esc Back",
    "Up/Down Select  Enter Toggle  Esc Done",
    "^v Select  <> Change  D Delete  Esc Back",
    "Delete local data?",
    "Settings and history will be removed, then the app will exit.",
    "Recovery required",
    "Local data cannot be opened. Exit without changes or delete all?",
    "Delete",
    "Local storage could not be saved.",
    "No error",
    "The messenger is not initialized.",
    "The messenger is already initialized.",
    "The user ID is invalid.",
    "The post body is invalid.",
    "Too many mentions.",
    "The same person is mentioned more than once.",
    "The reply target is no longer available.",
    "The outbound queue is full.",
    "The local timeline is full.",
    "The sender sequence is exhausted.",
    "The local timeline order is exhausted.",
    "Secure random data is unavailable.",
    "The generated message ID already exists.",
    "The post is invalid.",
    "This post is already in the timeline.",
    "A different post uses the same message ID.",
    "The requested post was not found.",
    "The requested post is not a local post.",
    "That status change is not allowed.",
    "That character cannot be inserted.",
    "An unknown error occurred.",
    "Wi-Fi LAN ready",
    "Queued for Wi-Fi LAN broadcast. Peer delivery is unconfirmed.",
    "WIFI LAN / H Exit",
    "Wi-Fi LAN ready / no peer delivery confirmation",
    "Talk",
    "Settings",
    "Skip title on startup",
    "ON",
    "OFF",
    "Up/Down Select  Enter Open  H Exit",
};

constexpr TranslationTable kJapanese{
    "LoRa Messenger",
    "タイムライン",
    "投稿の詳細",
    "新しい投稿",
    "メンション",
    "設定",
    "まだメッセージはありません",
    "ローカル履歴にユーザーがいません",
    "ユーザーID",
    "言語",
    "ローカルに保存済み",
    "無線は無効です",
    "日本LoRa使用可",
    "状態",
    "エラー",
    "ローカル保存済みです。無線は無効で、配信確認はありません。",
    "日本LoRa送信待ちです。配信確認はありません。",
    "その投稿はローカル履歴にありません。",
    "下書きを破棄しますか？",
    "未送信の下書きは失われます。",
    "LoRa Messengerを終了しますか？",
    "ローカル設定と履歴は残ります。",
    "キャンセル",
    "破棄",
    "終了",
    "受信",
    "送信待ち",
    "送信済み",
    "失敗",
    "不明",
    "元の投稿はありません",
    "メンションは4人までです。",
    "ローカル",
    "ローカル / H 終了",
    "日本LoRa / H 終了",
    "無線無効 / 配信確認なし",
    "日本LoRa使用可 / 配信確認なし",
    "残りバイト",
    "返信",
    "あなた宛て",
    "識別子",
    "上下 選択  Ent 開く  N 新規  R 返信  M @  S 設定",
    "上下 移動  R 返信  M @  Esc 戻る",
    "入力  Enter 送信  Tab @  Esc 戻る",
    "上下 選択  Enter 切替  Esc 完了",
    "上下 選択  左右 切替  D 削除  Esc 戻る",
    "ローカルデータを削除？",
    "設定と履歴を削除して、アプリを終了します。",
    "復旧が必要です",
    "ローカルデータを開けません。変更せず終了、または全削除します。",
    "削除",
    "ローカル保存に失敗しました。",
    "エラーはありません",
    "メッセンジャーが初期化されていません。",
    "メッセンジャーはすでに初期化されています。",
    "ユーザーIDが無効です。",
    "投稿本文が無効です。",
    "メンションが多すぎます。",
    "同じユーザーが重複しています。",
    "返信先は利用できません。",
    "送信待ちキューが満杯です。",
    "ローカルタイムラインが満杯です。",
    "送信シーケンスを発行できません。",
    "ローカル順序を発行できません。",
    "安全な乱数を利用できません。",
    "生成したメッセージIDがすでに存在します。",
    "投稿が無効です。",
    "この投稿はすでにタイムラインにあります。",
    "同じメッセージIDの異なる投稿があります。",
    "指定した投稿が見つかりません。",
    "指定した投稿はローカル投稿ではありません。",
    "その状態変更は許可されていません。",
    "その文字は入力できません。",
    "不明なエラーが発生しました。",
    "Wi-Fi LAN使用可",
    "Wi-Fi LAN送信待ちです。配信確認はありません。",
    "WIFI LAN / H 終了",
    "Wi-Fi LAN使用可 / 配信確認なし",
    "Talk",
    "設定",
    "Skip title",
    "ON",
    "OFF",
    "上下 選択  Enter 開く  H 終了",
};

constexpr TranslationTable kSimplifiedChinese{
    "LoRa Messenger",
    "时间线",
    "帖子详情",
    "新帖子",
    "提及",
    "设置",
    "还没有消息",
    "本地历史中没有用户",
    "用户ID",
    "语言",
    "已保存到本地",
    "无线电已禁用",
    "日本LoRa可用",
    "状态",
    "错误",
    "已在本地排队。无线电已禁用，也没有送达确认。",
    "已在日本LoRa排队。没有送达确认。",
    "该帖子已不在本地历史中。",
    "放弃草稿？",
    "未发送的草稿将丢失。",
    "退出LoRa Messenger？",
    "本地设置和历史记录会保留。",
    "取消",
    "放弃",
    "退出",
    "已接收",
    "排队中",
    "已广播",
    "失败",
    "未知",
    "原帖子不可用",
    "一条帖子最多可提及4人。",
    "本地",
    "本地 / H 退出",
    "日本LoRa / H 退出",
    "无线电已禁用 / 无送达确认",
    "日本LoRa可用 / 无送达确认",
    "剩余字节",
    "回复",
    "提及了你",
    "身份标识",
    "上下 选择  Ent 打开  N 新建  R 回复  M @  S 设置",
    "上下 移动  R 回复  M @  Esc 返回",
    "输入  Enter 发送  Tab @  Esc 返回",
    "上下 选择  Enter 切换  Esc 完成",
    "上下 选择  左右 切换  D 删除  Esc 返回",
    "删除本地数据？",
    "设置和历史记录将被删除，然后应用会退出。",
    "需要恢复",
    "无法打开本地数据。退出且不更改，或全部删除？",
    "删除",
    "无法保存本地数据。",
    "没有错误",
    "消息程序尚未初始化。",
    "消息程序已经初始化。",
    "用户ID无效。",
    "帖子正文无效。",
    "提及人数过多。",
    "同一用户被重复提及。",
    "回复目标已不可用。",
    "发送队列已满。",
    "本地时间线已满。",
    "发送序号已耗尽。",
    "本地时间线顺序已耗尽。",
    "无法取得安全随机数据。",
    "生成的消息ID已经存在。",
    "帖子无效。",
    "该帖子已在时间线中。",
    "另一个帖子使用了相同的消息ID。",
    "找不到指定的帖子。",
    "指定的帖子不是本地帖子。",
    "不允许该状态变更。",
    "无法插入该字符。",
    "发生未知错误。",
    "Wi-Fi LAN可用",
    "已在Wi-Fi LAN排队。没有送达确认。",
    "WIFI LAN / H 退出",
    "Wi-Fi LAN可用 / 无送达确认",
    "Talk",
    "设置",
    "Skip title",
    "ON",
    "OFF",
    "上下 选择  Enter 打开  H 退出",
};

constexpr std::array<TranslationTable, kLocaleCount> kTranslations{
    kEnglish,
    kJapanese,
    kSimplifiedChinese,
};

constexpr std::array<std::string_view, kLocaleCount> kLocaleCodes{
    "en",
    "ja",
    "zh-Hans",
};

constexpr std::array<std::string_view, kLocaleCount> kLocaleNames{
    "English",
    "日本語",
    "简体中文",
};

constexpr std::size_t index(Locale locale) noexcept {
    return static_cast<std::size_t>(locale);
}

constexpr std::size_t index(StringId id) noexcept {
    return static_cast<std::size_t>(id);
}

} // namespace

std::string_view locale_code(Locale locale) noexcept {
    const auto locale_index = index(locale);
    return locale_index < kLocaleCount ? kLocaleCodes[locale_index]
                                       : kLocaleCodes[index(Locale::English)];
}

std::string_view locale_display_name(Locale locale) noexcept {
    const auto locale_index = index(locale);
    return locale_index < kLocaleCount ? kLocaleNames[locale_index]
                                       : kLocaleNames[index(Locale::English)];
}

std::string_view translate(Locale locale, StringId id) noexcept {
    const auto id_index = index(id);
    if (id_index >= kStringIdCount) {
        return kEnglish[index(StringId::UnknownError)];
    }

    const auto locale_index = index(locale);
    if (locale_index < kLocaleCount) {
        const auto translated = kTranslations[locale_index][id_index];
        if (!translated.empty()) {
            return translated;
        }
    }
    return kEnglish[id_index];
}

bool translations_complete() noexcept {
    for (const auto& table : kTranslations) {
        for (const auto value : table) {
            if (value.empty()) {
                return false;
            }
        }
    }
    return true;
}

StringId command_error_string_id(application::CommandError error) noexcept {
    using application::CommandError;
    switch (error) {
        case CommandError::None: return StringId::ErrorNone;
        case CommandError::NotInitialized: return StringId::ErrorNotInitialized;
        case CommandError::AlreadyInitialized: return StringId::ErrorAlreadyInitialized;
        case CommandError::InvalidUserId: return StringId::ErrorInvalidUserId;
        case CommandError::InvalidBody: return StringId::ErrorInvalidBody;
        case CommandError::TooManyMentions: return StringId::ErrorTooManyMentions;
        case CommandError::DuplicateMention: return StringId::ErrorDuplicateMention;
        case CommandError::ReplyParentUnavailable:
            return StringId::ErrorReplyParentUnavailable;
        case CommandError::QueueFull: return StringId::ErrorQueueFull;
        case CommandError::TimelineFull: return StringId::ErrorTimelineFull;
        case CommandError::SequenceExhausted: return StringId::ErrorSequenceExhausted;
        case CommandError::OrderExhausted: return StringId::ErrorOrderExhausted;
        case CommandError::RandomUnavailable: return StringId::ErrorRandomUnavailable;
        case CommandError::MessageIdCollision: return StringId::ErrorMessageIdCollision;
        case CommandError::InvalidPost: return StringId::ErrorInvalidPost;
        case CommandError::DuplicatePost: return StringId::ErrorDuplicatePost;
        case CommandError::ConflictingPost: return StringId::ErrorConflictingPost;
        case CommandError::MessageNotFound: return StringId::ErrorMessageNotFound;
        case CommandError::NotLocalPost: return StringId::ErrorNotLocalPost;
        case CommandError::InvalidTransition: return StringId::ErrorInvalidTransition;
        case CommandError::PersistenceUnavailable:
            return StringId::ErrorPersistenceUnavailable;
    }
    return StringId::UnknownError;
}

} // namespace lora::viewmodel
