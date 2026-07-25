/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/messenger_view_model.h"

#include "test_fakes.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

lora::model::Identity identity(std::uint8_t discriminator,
                               const char* user = "local") {
    auto user_id = lora::core::UserId::create(user);
    return lora::model::Identity::restore(
        lora::test::make_install_id(discriminator),
        std::move(user_id).value());
}

lora::model::PostPayload received(
    std::uint8_t message,
    std::uint8_t sender,
    std::string body = "hello",
    std::string user = "peer",
    std::optional<lora::core::MessageId> reply_to = std::nullopt) {
    return lora::test::make_post(message, sender, 1, std::move(body), {},
                                 std::move(reply_to), std::nullopt,
                                 std::move(user));
}

lora::viewmodel::KeyEvent key(lora::viewmodel::UiKey value) {
    return {value, 0};
}

lora::viewmodel::KeyEvent character(char32_t value) {
    return {lora::viewmodel::UiKey::Character, value};
}

void type(lora::viewmodel::MessengerViewModel& view_model,
          std::u32string_view value) {
    for (const auto codepoint : value) {
        view_model.handle(character(codepoint));
    }
}

class FakeUiSettingsCommit final
    : public lora::viewmodel::IUiSettingsCommit {
public:
    bool persist_locale(lora::viewmodel::Locale locale) noexcept override {
        ++calls;
        requested = locale;
        return succeeds;
    }

    bool persist_skip_title(bool skip_title) noexcept override {
        ++skip_calls;
        requested_skip_title = skip_title;
        return succeeds;
    }

    bool succeeds{true};
    std::size_t calls{0};
    std::size_t skip_calls{0};
    lora::viewmodel::Locale requested{
        lora::viewmodel::Locale::English};
    bool requested_skip_title{false};
};

} // namespace

int main() {
    using lora::application::MessengerState;
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::viewmodel::ComposeSnapshot;
    using lora::viewmodel::DetailSnapshot;
    using lora::viewmodel::Locale;
    using lora::viewmodel::MenuItem;
    using lora::viewmodel::MenuSnapshot;
    using lora::viewmodel::MentionsSnapshot;
    using lora::viewmodel::MessengerViewModel;
    using lora::viewmodel::ModalId;
    using lora::viewmodel::ReplyAvailability;
    using lora::viewmodel::ScreenId;
    using lora::viewmodel::SettingsSnapshot;
    using lora::viewmodel::TimelineSnapshot;
    using lora::viewmodel::UiKey;
    lora::test::Runner runner;

    runner.run(
        "title menu routes to Talk and Settings and persists skip flag",
        [&] {
            lora::test::ScriptedRandom random;
            lora::test::FakeClock clock;
            MessengerState state(random, clock);
            REQUIRE(state.restore_identity(identity(10)).ok());
            FakeUiSettingsCommit commit;
            MessengerViewModel view_model(
                state, Locale::English, &commit, false, false);

            CHECK_EQ(view_model.screen(), ScreenId::Menu);
            auto snapshot = view_model.snapshot();
            auto* menu = std::get_if<MenuSnapshot>(&snapshot.page);
            REQUIRE(menu != nullptr);
            CHECK_EQ(menu->selected, MenuItem::Talk);

            REQUIRE(
                view_model.handle(key(UiKey::Down)).render_required);
            snapshot = view_model.snapshot();
            menu = std::get_if<MenuSnapshot>(&snapshot.page);
            REQUIRE(menu != nullptr);
            CHECK_EQ(menu->selected, MenuItem::Settings);
            REQUIRE(
                view_model.handle(key(UiKey::Enter)).render_required);
            CHECK_EQ(view_model.screen(), ScreenId::Settings);
            REQUIRE(
                view_model.handle(key(UiKey::Escape)).render_required);
            CHECK_EQ(view_model.screen(), ScreenId::Menu);

            REQUIRE(
                view_model.handle(key(UiKey::Up)).render_required);
            REQUIRE(
                view_model.handle(key(UiKey::Enter)).render_required);
            CHECK_EQ(view_model.screen(), ScreenId::Timeline);
            REQUIRE(
                view_model.handle(character(U's')).render_required);
            CHECK_EQ(view_model.screen(), ScreenId::Settings);
            REQUIRE(
                view_model.handle(key(UiKey::Down)).render_required);
            snapshot = view_model.snapshot();
            auto* settings =
                std::get_if<SettingsSnapshot>(&snapshot.page);
            REQUIRE(settings != nullptr);
            CHECK(!settings->skip_title);
            REQUIRE(
                view_model.handle(key(UiKey::Enter)).render_required);
            CHECK_EQ(commit.skip_calls, 1U);
            CHECK(commit.requested_skip_title);
            snapshot = view_model.snapshot();
            settings = std::get_if<SettingsSnapshot>(&snapshot.page);
            REQUIRE(settings != nullptr);
            CHECK(settings->skip_title);
            REQUIRE(
                view_model.handle(key(UiKey::Escape)).render_required);
            CHECK_EQ(view_model.screen(), ScreenId::Timeline);
        });

    runner.run("skip title starts directly in Talk", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(
            state, Locale::English, nullptr, false, true);
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
    });

    runner.run("timeline navigation is newest-first and snapshots stay bounded", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        for (std::uint8_t index = 0; index < 5; ++index) {
            std::string body = index == 4 ? std::string(30, 'x') : "body";
            REQUIRE(state.accept_received(received(
                static_cast<std::uint8_t>(20 + index),
                static_cast<std::uint8_t>(40 + index), std::move(body),
                "peer" + std::to_string(index))).ok());
        }

        MessengerViewModel view_model(state);
        auto snapshot = view_model.snapshot();
        auto* timeline = std::get_if<TimelineSnapshot>(&snapshot.page);
        REQUIRE(timeline != nullptr);
        CHECK_EQ(timeline->total_rows, 5U);
        CHECK_EQ(timeline->rows.size(), lora::viewmodel::kVisibleTimelineRows);
        REQUIRE(timeline->selected_message_id.has_value());
        CHECK_EQ(*timeline->selected_message_id, lora::test::make_message_id(24));
        CHECK(timeline->rows.front().selected);

        CHECK(view_model.handle(key(UiKey::Down)).render_required);
        REQUIRE(view_model.selected_message_id().has_value());
        CHECK_EQ(*view_model.selected_message_id(),
                 lora::test::make_message_id(23));
        CHECK(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Detail);
        CHECK(view_model.handle(key(UiKey::Escape)).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
    });

    runner.run("duplicate display names request UUID disambiguation", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        REQUIRE(state.accept_received(received(30, 40, "one", "sam")).ok());
        REQUIRE(state.accept_received(received(31, 41, "two", "sam")).ok());

        MessengerViewModel view_model(state);
        auto snapshot = view_model.snapshot();
        auto* timeline = std::get_if<TimelineSnapshot>(&snapshot.page);
        REQUIRE(timeline != nullptr);
        REQUIRE(timeline->rows.size() == 2U);
        CHECK(timeline->rows[0].show_sender_suffix);
        CHECK(timeline->rows[1].show_sender_suffix);
        CHECK_NE(timeline->rows[0].sender_suffix,
                 timeline->rows[1].sender_suffix);
    });

    runner.run("compose treats shortcut letters as text and leaves posts queued", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(50);
        lora::test::FakeClock clock(1'700'000'000);
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);

        CHECK(view_model.handle(character(U'n')).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        type(view_model, U"nRmS");
        auto compose_view = view_model.snapshot();
        auto* compose = std::get_if<ComposeSnapshot>(&compose_view.page);
        REQUIRE(compose != nullptr);
        CHECK_EQ(compose->body, "nRmS");
        CHECK_EQ(compose->remaining_bytes, 156U);

        CHECK(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
        CHECK_EQ(view_model.modal(), ModalId::Status);
        REQUIRE(view_model.selected_message_id().has_value());
        const auto* entry = state.timeline().find(*view_model.selected_message_id());
        REQUIRE(entry != nullptr);
        CHECK_EQ(entry->post.body().value(), "nRmS");
        const auto* delivery = std::get_if<LocalDelivery>(&entry->origin);
        REQUIRE(delivery != nullptr);
        CHECK_EQ(delivery->state, LocalDeliveryState::Queued);
        CHECK_EQ(state.timeline().queued_count(), 1U);
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(clock.call_count(), 1U);
        CHECK(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(view_model.modal(), ModalId::None);
    });

    runner.run("radio-ready compose discloses broadcast without delivery confirmation", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(52);
        lora::test::FakeClock clock(1'700'000'001);
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        view_model.set_radio_ready(true);

        CHECK(view_model.snapshot().radio_ready);
        view_model.handle(character(U'n'));
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Enter));

        const auto snapshot = view_model.snapshot();
        CHECK_EQ(view_model.modal(), ModalId::Status);
        CHECK_EQ(
            snapshot.modal.message,
            "Queued for JP LoRa broadcast. Peer delivery is unconfirmed.");
    });

    runner.run("Wi-Fi LAN compose uses LAN status without claiming delivery", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(53);
        lora::test::FakeClock clock(1'700'000'002);
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        view_model.set_transport_status(
            lora::viewmodel::TransportStatus::WifiLan);

        auto snapshot = view_model.snapshot();
        CHECK(snapshot.radio_ready);
        CHECK_EQ(
            snapshot.transport_status,
            lora::viewmodel::TransportStatus::WifiLan);
        view_model.handle(character(U'n'));
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Enter));

        snapshot = view_model.snapshot();
        CHECK_EQ(view_model.modal(), ModalId::Status);
        CHECK_EQ(
            snapshot.modal.message,
            "Queued for Wi-Fi LAN broadcast. Peer delivery is unconfirmed.");
    });

    runner.run("empty and over-limit composition surface errors without mutation", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(51);
        lora::test::FakeClock clock(10);
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        REQUIRE(view_model.handle(character(U'N')).render_required);

        REQUIRE(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        CHECK_EQ(view_model.modal(), ModalId::Error);
        CHECK(state.timeline().empty());
        CHECK_EQ(random.call_count(), 0U);
        CHECK(view_model.handle(key(UiKey::Escape)).render_required);

        for (std::size_t index = 0; index < 160; ++index) {
            view_model.handle(character(U'x'));
        }
        auto snapshot = view_model.snapshot();
        auto* compose = std::get_if<ComposeSnapshot>(&snapshot.page);
        REQUIRE(compose != nullptr);
        CHECK_EQ(compose->body.size(), 160U);
        CHECK_EQ(compose->remaining_bytes, 0U);
        view_model.handle(character(U'y'));
        CHECK_EQ(view_model.modal(), ModalId::Error);
        CHECK_EQ(state.timeline().size(), 0U);
        view_model.handle(key(UiKey::Escape));
        snapshot = view_model.snapshot();
        compose = std::get_if<ComposeSnapshot>(&snapshot.page);
        REQUIRE(compose != nullptr);
        CHECK_EQ(compose->body.size(), 160U);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
    });

    runner.run("dirty discard defaults to cancel and clean compose backs out", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);

        view_model.handle(character(U'n'));
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.modal(), ModalId::Discard);
        auto snapshot = view_model.snapshot();
        CHECK(!snapshot.modal.confirm_selected);
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.modal(), ModalId::None);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        snapshot = view_model.snapshot();
        auto* compose = std::get_if<ComposeSnapshot>(&snapshot.page);
        REQUIRE(compose != nullptr);
        CHECK_EQ(compose->body, "x");

        view_model.handle(key(UiKey::Escape));
        view_model.handle(key(UiKey::Right));
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
        CHECK_EQ(view_model.modal(), ModalId::None);

        view_model.handle(character(U'n'));
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
        CHECK_EQ(view_model.modal(), ModalId::None);
    });

    runner.run("Home exit confirmation defaults to cancel and preserves draft", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        view_model.handle(character(U'n'));
        type(view_model, U"draft");

        view_model.handle(key(UiKey::Home));
        CHECK_EQ(view_model.modal(), ModalId::Exit);
        CHECK(!view_model.snapshot().modal.confirm_selected);
        const auto cancelled = view_model.handle(key(UiKey::Enter));
        CHECK(!cancelled.exit_approved);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        auto snapshot = view_model.snapshot();
        auto* compose = std::get_if<ComposeSnapshot>(&snapshot.page);
        REQUIRE(compose != nullptr);
        CHECK_EQ(compose->body, "draft");

        view_model.handle(key(UiKey::Home));
        view_model.handle(key(UiKey::Right));
        const auto approved = view_model.handle(key(UiKey::Enter));
        CHECK(approved.exit_approved);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
    });

    runner.run("mention picker deduplicates retained peers excludes self and caps four", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(90);
        lora::test::FakeClock clock(20);
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        REQUIRE(state.accept_received(received(40, 20, "a", "peer-a")).ok());
        REQUIRE(state.accept_received(received(41, 20, "a2", "peer-a-new")).ok());
        for (std::uint8_t index = 0; index < 5; ++index) {
            REQUIRE(state.accept_received(received(
                static_cast<std::uint8_t>(42 + index),
                static_cast<std::uint8_t>(21 + index), "body",
                "peer-" + std::to_string(index))).ok());
        }
        REQUIRE(state.accept_received(received(48, 10, "own wire", "local")).ok());

        MessengerViewModel view_model(state);
        view_model.handle(character(U'n'));
        view_model.handle(key(UiKey::Tab));
        CHECK_EQ(view_model.screen(), ScreenId::Mentions);
        auto snapshot = view_model.snapshot();
        auto* mentions = std::get_if<MentionsSnapshot>(&snapshot.page);
        REQUIRE(mentions != nullptr);
        CHECK_EQ(mentions->total_options, 6U);
        CHECK_EQ(mentions->options.size(), lora::viewmodel::kVisibleMentionRows);

        for (std::size_t index = 0; index < 4; ++index) {
            view_model.handle(key(UiKey::Enter));
            if (index + 1 < 4) {
                view_model.handle(key(UiKey::Down));
            }
        }
        snapshot = view_model.snapshot();
        mentions = std::get_if<MentionsSnapshot>(&snapshot.page);
        REQUIRE(mentions != nullptr);
        CHECK_EQ(mentions->selected_count, 4U);
        view_model.handle(key(UiKey::Down));
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.modal(), ModalId::Error);
        view_model.handle(key(UiKey::Escape));
        snapshot = view_model.snapshot();
        mentions = std::get_if<MentionsSnapshot>(&snapshot.page);
        REQUIRE(mentions != nullptr);
        CHECK_EQ(mentions->selected_count, 4U);

        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Enter));
        REQUIRE(view_model.selected_message_id().has_value());
        const auto* created = state.timeline().find(*view_model.selected_message_id());
        REQUIRE(created != nullptr);
        CHECK_EQ(created->post.mentions().size(), 4U);
        CHECK_EQ(state.timeline().queued_count(), 1U);
    });

    runner.run("empty mention history remains keyboard escapable", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        view_model.handle(character(U'n'));
        view_model.handle(key(UiKey::Tab));
        auto snapshot = view_model.snapshot();
        auto* mentions = std::get_if<MentionsSnapshot>(&snapshot.page);
        REQUIRE(mentions != nullptr);
        CHECK_EQ(mentions->total_options, 0U);
        CHECK(mentions->options.empty());
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
    });

    runner.run("evicted detail selection returns to timeline with status", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock, 2);
        REQUIRE(state.restore_identity(identity(10)).ok());
        const auto oldest = received(60, 70, "oldest", "old");
        REQUIRE(state.accept_received(oldest).ok());
        REQUIRE(state.accept_received(received(61, 71, "newer", "new")).ok());

        MessengerViewModel view_model(state);
        view_model.handle(key(UiKey::Down));
        REQUIRE(view_model.selected_message_id().has_value());
        CHECK_EQ(*view_model.selected_message_id(), oldest.message_id());
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.screen(), ScreenId::Detail);
        const auto inserted = state.accept_received(received(62, 72, "newest", "latest"));
        REQUIRE(inserted.ok());
        REQUIRE(inserted.evicted_message_id.has_value());
        CHECK_EQ(*inserted.evicted_message_id, oldest.message_id());

        view_model.refresh();
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
        CHECK_EQ(view_model.modal(), ModalId::Status);
        REQUIRE(view_model.selected_message_id().has_value());
        CHECK_EQ(*view_model.selected_message_id(), lora::test::make_message_id(62));
    });

    runner.run("reply compose reports a parent evicted after opening", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(91);
        lora::test::FakeClock clock(30);
        MessengerState state(random, clock, 2);
        REQUIRE(state.restore_identity(identity(10)).ok());
        const auto parent = received(70, 80, "parent", "parent-user");
        REQUIRE(state.accept_received(parent).ok());
        REQUIRE(state.accept_received(received(71, 81, "other", "other-user")).ok());

        MessengerViewModel view_model(state);
        view_model.handle(key(UiKey::Down));
        view_model.handle(character(U'r'));
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        auto snapshot = view_model.snapshot();
        auto* compose = std::get_if<ComposeSnapshot>(&snapshot.page);
        REQUIRE(compose != nullptr);
        REQUIRE(compose->reply_to.has_value());
        CHECK_EQ(*compose->reply_to, parent.message_id());

        REQUIRE(state.accept_received(received(72, 82, "replacement", "replacement-user")).ok());
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        CHECK_EQ(view_model.modal(), ModalId::Error);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.timeline().queued_count(), 0U);
    });

    runner.run("missing reply context and settings are represented without timestamps", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10, "local-user")).ok());
        REQUIRE(state.accept_received(received(
            80, 90, "orphan reply", "reply-user",
            lora::test::make_message_id(99))).ok());

        MessengerViewModel view_model(state);
        view_model.handle(key(UiKey::Enter));
        auto snapshot = view_model.snapshot();
        auto* detail = std::get_if<DetailSnapshot>(&snapshot.page);
        REQUIRE(detail != nullptr);
        CHECK_EQ(detail->reply, ReplyAvailability::Unavailable);
        CHECK(!detail->reply_preview.empty());
        view_model.handle(key(UiKey::Escape));
        view_model.handle(character(U's'));
        snapshot = view_model.snapshot();
        auto* settings = std::get_if<SettingsSnapshot>(&snapshot.page);
        REQUIRE(settings != nullptr);
        CHECK_EQ(settings->user_id, "local-user");
        CHECK(!settings->install_suffix.empty());
        CHECK_EQ(settings->locale_name, "English");
        CHECK(!settings->session_notice.empty());
        CHECK(!settings->radio_status.empty());
        CHECK(settings->radio_status.find("delivery") != std::string::npos);

        view_model.handle(key(UiKey::Left));
        CHECK_EQ(view_model.locale(), Locale::SimplifiedChinese);
        snapshot = view_model.snapshot();
        CHECK_EQ(snapshot.title, "设置");
        view_model.handle(key(UiKey::Right));
        CHECK_EQ(view_model.locale(), Locale::English);
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Timeline);
    });

    runner.run("detail scrolling uses exact content bounds without hidden overscroll", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());

        std::string many_lines;
        for (std::size_t index = 0; index < 80; ++index) {
            if (!many_lines.empty()) {
                many_lines.push_back('\n');
            }
            many_lines.push_back('a');
        }
        REQUIRE(many_lines.size() == 159U);
        REQUIRE(state.accept_received(received(81, 91, many_lines, "scroller")).ok());

        MessengerViewModel view_model(state);
        REQUIRE(view_model.handle(key(UiKey::Enter)).render_required);
        auto snapshot = view_model.snapshot();
        auto* detail = std::get_if<DetailSnapshot>(&snapshot.page);
        REQUIRE(detail != nullptr);
        CHECK_EQ(detail->scroll_line, 0U);
        CHECK_EQ(detail->maximum_scroll_line, 78U);
        CHECK_EQ(detail->visible_body, "a\na");

        for (std::size_t index = 0; index < detail->maximum_scroll_line; ++index) {
            CHECK(view_model.handle(key(UiKey::Down)).render_required);
        }
        CHECK(!view_model.handle(key(UiKey::Down)).render_required);
        snapshot = view_model.snapshot();
        detail = std::get_if<DetailSnapshot>(&snapshot.page);
        REQUIRE(detail != nullptr);
        CHECK_EQ(detail->scroll_line, detail->maximum_scroll_line);
        CHECK_EQ(detail->visible_body, "a\na");
        CHECK(view_model.handle(key(UiKey::Up)).render_required);
        CHECK_EQ(std::get<DetailSnapshot>(view_model.snapshot().page).scroll_line, 77U);

        view_model.handle(key(UiKey::Escape));
        REQUIRE(state.accept_received(received(82, 92, "short", "shorter")).ok());
        view_model.refresh();
        REQUIRE(view_model.handle(key(UiKey::Up)).render_required);
        REQUIRE(view_model.handle(key(UiKey::Enter)).render_required);
        snapshot = view_model.snapshot();
        detail = std::get_if<DetailSnapshot>(&snapshot.page);
        REQUIRE(detail != nullptr);
        CHECK_EQ(detail->maximum_scroll_line, 0U);
        CHECK(!view_model.handle(key(UiKey::Down)).render_required);
        CHECK_EQ(std::get<DetailSnapshot>(view_model.snapshot().page).scroll_line, 0U);
    });

    runner.run("detail wrapping conservatively bounds wide ASCII glyphs", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        REQUIRE(state.accept_received(
            received(87, 97, std::string(52, 'O'), "wide-peer")).ok());
        MessengerViewModel view_model(state);
        view_model.handle(key(UiKey::Enter));

        auto detail = std::get<DetailSnapshot>(view_model.snapshot().page);
        CHECK_EQ(detail.maximum_scroll_line, 2U);
        CHECK_EQ(detail.visible_body, std::string(13, 'O') + "\n" +
                                      std::string(13, 'O'));
        CHECK(view_model.handle(key(UiKey::Down)).render_required);
        CHECK(view_model.handle(key(UiKey::Down)).render_required);
        CHECK(!view_model.handle(key(UiKey::Down)).render_required);
        detail = std::get<DetailSnapshot>(view_model.snapshot().page);
        CHECK_EQ(detail.scroll_line, 2U);
        CHECK_EQ(detail.visible_body, std::string(13, 'O') + "\n" +
                                      std::string(13, 'O'));
    });

    runner.run("non-entry shortcuts work from detail and settings with safe return", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        REQUIRE(state.accept_received(received(83, 93, "target", "peer")).ok());
        MessengerViewModel view_model(state);

        view_model.handle(key(UiKey::Enter));
        REQUIRE(view_model.screen() == ScreenId::Detail);
        CHECK(view_model.handle(character(U'n')).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Detail);

        CHECK(view_model.handle(character(U's')).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Settings);
        CHECK(view_model.handle(character(U'n')).render_required);
        CHECK_EQ(view_model.screen(), ScreenId::Compose);
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Settings);
        view_model.handle(key(UiKey::Escape));
        CHECK_EQ(view_model.screen(), ScreenId::Detail);
    });

    runner.run("selected evicted mentions remain removable from the picker", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock, 2);
        REQUIRE(state.restore_identity(identity(10)).ok());
        REQUIRE(state.accept_received(received(84, 94, "old", "old-peer")).ok());
        REQUIRE(state.accept_received(received(85, 95, "new", "new-peer")).ok());
        MessengerViewModel view_model(state);

        view_model.handle(character(U'n'));
        view_model.handle(key(UiKey::Tab));
        view_model.handle(key(UiKey::Down));
        view_model.handle(key(UiKey::Enter));
        auto mentions = std::get<MentionsSnapshot>(view_model.snapshot().page);
        CHECK_EQ(mentions.selected_count, 1U);
        view_model.handle(key(UiKey::Escape));

        const auto replacement = state.accept_received(
            received(86, 96, "replacement", "replacement-peer"));
        REQUIRE(replacement.ok());
        REQUIRE(replacement.evicted_message_id.has_value());
        CHECK_EQ(*replacement.evicted_message_id, lora::test::make_message_id(84));

        view_model.handle(key(UiKey::Tab));
        mentions = std::get<MentionsSnapshot>(view_model.snapshot().page);
        CHECK_EQ(mentions.total_options, 3U);
        REQUIRE(mentions.options.size() == 3U);
        CHECK(mentions.options[2].selected);
        CHECK(mentions.options[2].cursor);
        view_model.handle(key(UiKey::Enter));
        mentions = std::get<MentionsSnapshot>(view_model.snapshot().page);
        CHECK_EQ(mentions.selected_count, 0U);
    });

    runner.run("uninitialized compose maps command failure to localized error", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        MessengerViewModel view_model(state, Locale::Japanese);
        view_model.handle(character(U'n'));
        view_model.handle(character(U'x'));
        view_model.handle(key(UiKey::Enter));
        CHECK_EQ(view_model.modal(), ModalId::Error);
        const auto snapshot = view_model.snapshot();
        CHECK_EQ(snapshot.modal.message,
                 "メッセンジャーが初期化されていません。");
        CHECK(state.timeline().empty());
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
    });

    runner.run("locale changes become visible only after durable commit", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        FakeUiSettingsCommit commit;
        MessengerViewModel view_model(
            state, Locale::English, &commit, false, false);
        view_model.handle(character(U's'));
        REQUIRE(view_model.screen() == ScreenId::Settings);

        commit.succeeds = false;
        CHECK(view_model.handle(key(UiKey::Right)).render_required);
        CHECK_EQ(view_model.locale(), Locale::English);
        CHECK_EQ(view_model.modal(), ModalId::Error);
        CHECK_EQ(commit.calls, 1U);
        CHECK_EQ(commit.requested, Locale::Japanese);

        view_model.handle(key(UiKey::Enter));
        commit.succeeds = true;
        CHECK(view_model.handle(key(UiKey::Right)).render_required);
        CHECK_EQ(view_model.locale(), Locale::Japanese);
        CHECK_EQ(commit.calls, 2U);

        CHECK(view_model.handle(key(UiKey::Down)).render_required);
        commit.succeeds = false;
        CHECK(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(view_model.modal(), ModalId::Error);
        CHECK_EQ(commit.skip_calls, 1U);
        CHECK(!std::get<SettingsSnapshot>(
                   view_model.snapshot().page).skip_title);

        view_model.handle(key(UiKey::Enter));
        commit.succeeds = true;
        CHECK(view_model.handle(key(UiKey::Enter)).render_required);
        CHECK_EQ(commit.skip_calls, 2U);
        CHECK(std::get<SettingsSnapshot>(
                  view_model.snapshot().page).skip_title);
    });

    runner.run("delete confirmation defaults to cancel and emits explicit action", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.restore_identity(identity(10)).ok());
        MessengerViewModel view_model(state);
        view_model.handle(character(U's'));

        CHECK(view_model.handle(character(U'd')).render_required);
        CHECK_EQ(view_model.modal(), ModalId::DeleteData);
        auto modal = view_model.snapshot().modal;
        CHECK(!modal.confirm_selected);
        const auto cancelled = view_model.handle(key(UiKey::Enter));
        CHECK(!cancelled.exit_approved);
        CHECK(!cancelled.delete_data_approved);
        CHECK_EQ(view_model.modal(), ModalId::None);

        view_model.handle(character(U'd'));
        view_model.handle(key(UiKey::Right));
        const auto confirmed = view_model.handle(key(UiKey::Enter));
        CHECK(!confirmed.exit_approved);
        CHECK(confirmed.delete_data_approved);
    });

    runner.run("recovery blocks normal UI and preserves data on default exit", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        MessengerViewModel exit_view(
            state, Locale::English, nullptr, true);
        CHECK_EQ(exit_view.modal(), ModalId::Recovery);
        const auto exit_action = exit_view.handle(key(UiKey::Enter));
        CHECK(exit_action.exit_approved);
        CHECK(!exit_action.delete_data_approved);
        CHECK_EQ(exit_view.modal(), ModalId::Recovery);

        MessengerViewModel delete_view(
            state, Locale::English, nullptr, true);
        delete_view.handle(key(UiKey::Right));
        const auto delete_action = delete_view.handle(key(UiKey::Enter));
        CHECK(!delete_action.exit_approved);
        CHECK(delete_action.delete_data_approved);
    });

    return runner.finish();
}
