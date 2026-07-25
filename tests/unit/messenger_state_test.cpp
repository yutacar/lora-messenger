/*
 * SPDX-License-Identifier: MIT
 */

#include "application/messenger_state.h"

#include "test_fakes.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

lora::model::Identity restored_identity(std::uint8_t install_discriminator,
                                        const char* user,
                                        std::uint64_t last_sequence = 0) {
    auto validated = lora::core::UserId::create(user);
    return lora::model::Identity::restore(
        lora::test::make_install_id(install_discriminator),
        std::move(validated).value(), last_sequence);
}

} // namespace

int main() {
    using lora::application::CommandError;
    using lora::application::MessengerState;
    using lora::application::MessengerStatus;
    using lora::core::TextError;
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::model::PostDraft;
    using lora::model::ReceivedOrigin;
    lora::test::Runner runner;

    runner.run("new identity validation happens before random generation", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(10);
        lora::test::FakeClock clock(100);
        MessengerState state(random, clock);
        CHECK_EQ(state.status(), MessengerStatus::Uninitialized);
        CHECK(!state.identity().has_value());

        const auto invalid = state.initialize_new(" alice");
        CHECK_EQ(invalid.error, CommandError::InvalidUserId);
        CHECK_EQ(invalid.text_error, TextError::EdgeWhitespace);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.status(), MessengerStatus::Uninitialized);

        const auto initialized = state.initialize_new("alice");
        CHECK(initialized.ok());
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.status(), MessengerStatus::Ready);
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->install_id(), lora::test::make_install_id(10));
        CHECK_EQ(state.identity()->user_id().value(), "alice");

        const auto repeated = state.initialize_new("other");
        CHECK_EQ(repeated.error, CommandError::AlreadyInitialized);
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(state.identity()->user_id().value(), "alice");
    });

    runner.run("random failure leaves initialization retryable", [&] {
        lora::test::ScriptedRandom random;
        random.fail_next();
        lora::test::FakeClock clock;
        MessengerState state(random, clock);

        const auto failed = state.initialize_new("alice");
        CHECK_EQ(failed.error, CommandError::RandomUnavailable);
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(state.status(), MessengerStatus::Uninitialized);
        CHECK(!state.identity().has_value());

        random.push_seed(11);
        const auto retried = state.initialize_new("alice");
        CHECK(retried.ok());
        CHECK_EQ(random.call_count(), 2U);
        CHECK_EQ(state.status(), MessengerStatus::Ready);
    });

    runner.run("restore and rename do not use random or clock ports", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock(200);
        MessengerState state(random, clock);

        const auto before_restore = state.rename_user("bob");
        CHECK_EQ(before_restore.error, CommandError::NotInitialized);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);

        const auto restored = state.restore_identity(restored_identity(12, "alice", 7U));
        CHECK(restored.ok());
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 7U);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);

        const auto repeated_restore = state.restore_identity(
            restored_identity(13, "mallory", 99U));
        CHECK_EQ(repeated_restore.error, CommandError::AlreadyInitialized);
        CHECK_EQ(state.identity()->install_id(), lora::test::make_install_id(12));
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 7U);

        const auto invalid_rename = state.rename_user("bob\n");
        CHECK_EQ(invalid_rename.error, CommandError::InvalidUserId);
        CHECK_EQ(invalid_rename.text_error, TextError::ForbiddenCharacter);
        CHECK_EQ(state.identity()->user_id().value(), "alice");

        const auto renamed = state.rename_user("bob");
        CHECK(renamed.ok());
        CHECK_EQ(state.identity()->user_id().value(), "bob");
        CHECK_EQ(state.identity()->install_id(), lora::test::make_install_id(12));
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 7U);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
    });

    runner.run("compose snapshots identity, sequence, mentions, and optional clock", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(20);
        random.push_seed(21);
        random.push_seed(22);
        lora::test::FakeClock clock(1'700'000'001);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        const auto mention = lora::test::make_install_id(99);

        const auto first = state.compose(PostDraft{"hello", {mention}, std::nullopt});
        REQUIRE(first.ok());
        REQUIRE(first.message_id.has_value());
        CHECK_EQ(*first.message_id, lora::test::make_message_id(21));
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
        CHECK_EQ(state.timeline().size(), 1U);
        CHECK_EQ(state.timeline().queued_count(), 1U);
        const auto* first_entry = state.timeline().find(*first.message_id);
        REQUIRE(first_entry != nullptr);
        CHECK_EQ(first_entry->post.sender_id(), state.identity()->install_id());
        CHECK_EQ(first_entry->post.sender_sequence(), 1U);
        CHECK_EQ(first_entry->post.sender_user_id().value(), "alice");
        CHECK_EQ(first_entry->post.body().value(), "hello");
        REQUIRE(first_entry->post.mentions().size() == 1U);
        CHECK_EQ(first_entry->post.mentions().front(), mention);
        REQUIRE(first_entry->post.sender_time().has_value());
        CHECK_EQ(*first_entry->post.sender_time(), 1'700'000'001);
        const auto* first_local = std::get_if<LocalDelivery>(&first_entry->origin);
        REQUIRE(first_local != nullptr);
        CHECK_EQ(first_local->state, LocalDeliveryState::Queued);

        clock.set(1'700'000'002);
        REQUIRE(state.rename_user("bob").ok());
        const auto second = state.compose(PostDraft{"after rename", {}, std::nullopt});
        REQUIRE(second.ok());
        REQUIRE(second.message_id.has_value());
        const auto* second_entry = state.timeline().find(*second.message_id);
        REQUIRE(second_entry != nullptr);
        CHECK_EQ(second_entry->post.sender_sequence(), 2U);
        CHECK_EQ(second_entry->post.sender_user_id().value(), "bob");
        const auto* restored_first_entry =
            state.timeline().find(*first.message_id);
        REQUIRE(restored_first_entry != nullptr);
        CHECK_EQ(restored_first_entry->post.sender_user_id().value(), "alice");
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 2U);
        CHECK_EQ(random.call_count(), 3U);
        CHECK_EQ(clock.call_count(), 2U);
    });

    runner.run("an unavailable clock does not prevent local composition", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(23);
        random.push_seed(24);
        lora::test::FakeClock clock;
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        const auto composed = state.compose(PostDraft{"offline time", {}, std::nullopt});
        REQUIRE(composed.ok());
        REQUIRE(composed.message_id.has_value());
        const auto* entry = state.timeline().find(*composed.message_id);
        REQUIRE(entry != nullptr);
        CHECK(!entry->post.sender_time().has_value());
        CHECK_EQ(clock.call_count(), 1U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
    });

    runner.run("invalid drafts reject before ports and preserve all state", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(25);
        random.push_seed(26);
        lora::test::FakeClock clock(300);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        const auto calls_before = random.call_count();

        const auto empty = state.compose(PostDraft{"", {}, std::nullopt});
        CHECK_EQ(empty.error, CommandError::InvalidBody);
        CHECK_EQ(empty.text_error, TextError::Empty);

        const auto mention_a = lora::test::make_install_id(100);
        const auto mention_b = lora::test::make_install_id(101);
        const auto mention_c = lora::test::make_install_id(102);
        const auto mention_d = lora::test::make_install_id(103);
        const auto mention_e = lora::test::make_install_id(104);
        const auto too_many = state.compose(PostDraft{
            "valid", {mention_a, mention_b, mention_c, mention_d, mention_e}, std::nullopt});
        CHECK_EQ(too_many.error, CommandError::TooManyMentions);

        const auto duplicate = state.compose(
            PostDraft{"valid", {mention_a, mention_b, mention_a}, std::nullopt});
        CHECK_EQ(duplicate.error, CommandError::DuplicateMention);

        const auto missing_reply = state.compose(PostDraft{
            "valid", {}, lora::test::make_message_id(199)});
        CHECK_EQ(missing_reply.error, CommandError::ReplyParentUnavailable);

        CHECK_EQ(random.call_count(), calls_before);
        CHECK_EQ(random.remaining(), 1U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.timeline().size(), 0U);
        CHECK_EQ(state.timeline().last_assigned_order(), 0U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 0U);
    });

    runner.run("compose while uninitialized has no observable side effects", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(27);
        lora::test::FakeClock clock(400);
        MessengerState state(random, clock);
        const auto result = state.compose(PostDraft{"hello", {}, std::nullopt});
        CHECK_EQ(result.error, CommandError::NotInitialized);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK(state.timeline().empty());
        CHECK_EQ(state.timeline().last_assigned_order(), 0U);
    });

    runner.run("receive and delivery commands require initialized identity", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(28);
        lora::test::FakeClock clock(450);
        MessengerState state(random, clock);
        const auto received = lora::test::make_post(90, 91);

        const auto receive_result = state.accept_received(received);
        CHECK_EQ(receive_result.error, CommandError::NotInitialized);
        CHECK_EQ(state.mark_broadcast(received.message_id()).error,
                 CommandError::NotInitialized);
        CHECK_EQ(state.mark_failed(received.message_id()).error,
                 CommandError::NotInitialized);
        CHECK(state.timeline().empty());
        CHECK_EQ(state.timeline().last_assigned_order(), 0U);
        CHECK_EQ(random.call_count(), 0U);
        CHECK_EQ(clock.call_count(), 0U);
    });

    runner.run("message random failure and collision never consume sequence or clock", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(28);
        lora::test::FakeClock clock(500);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());

        random.fail_next();
        const auto unavailable = state.compose(PostDraft{"first", {}, std::nullopt});
        CHECK_EQ(unavailable.error, CommandError::RandomUnavailable);
        CHECK_EQ(random.call_count(), 2U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 0U);
        CHECK(state.timeline().empty());

        const auto received = lora::test::make_post(29, 60, 1U, "received");
        REQUIRE(state.accept_received(received).ok());
        random.push_seed(29);
        const auto collision = state.compose(PostDraft{"second", {}, std::nullopt});
        CHECK_EQ(collision.error, CommandError::MessageIdCollision);
        CHECK_EQ(random.call_count(), 3U);
        CHECK_EQ(clock.call_count(), 0U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 0U);
        CHECK_EQ(state.timeline().size(), 1U);
        CHECK_EQ(state.timeline().last_assigned_order(), 1U);
        REQUIRE(state.timeline().find(received.message_id()) != nullptr);
        CHECK_EQ(state.timeline().find(received.message_id())->post.body().value(), "received");

        random.push_seed(30);
        const auto recovered = state.compose(PostDraft{"third", {}, std::nullopt});
        CHECK(recovered.ok());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
        CHECK_EQ(clock.call_count(), 1U);
    });

    runner.run("the seventeenth queued post is rejected without consuming ports", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(40);
        for (std::uint8_t seed = 100; seed <= 116; ++seed) {
            random.push_seed(seed);
        }
        lora::test::FakeClock clock(600);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        std::vector<lora::core::MessageId> created;
        for (std::size_t index = 0; index < 16; ++index) {
            const auto result = state.compose(
                PostDraft{"queued " + std::to_string(index), {}, std::nullopt});
            CHECK(result.ok());
            if (result.message_id) {
                created.push_back(*result.message_id);
            }
        }
        REQUIRE(created.size() == 16U);
        CHECK_EQ(state.timeline().queued_count(), 16U);
        CHECK_EQ(state.timeline().size(), 16U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 16U);
        CHECK_EQ(random.call_count(), 17U);
        CHECK_EQ(clock.call_count(), 16U);
        CHECK_EQ(random.remaining(), 1U);

        const auto seventeenth = state.compose(PostDraft{"queue full", {}, std::nullopt});
        CHECK_EQ(seventeenth.error, CommandError::QueueFull);
        CHECK_EQ(state.timeline().queued_count(), 16U);
        CHECK_EQ(state.timeline().size(), 16U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 16U);
        CHECK_EQ(random.call_count(), 17U);
        CHECK_EQ(clock.call_count(), 16U);
        CHECK_EQ(random.remaining(), 1U);

        REQUIRE(state.mark_broadcast(created.front()).ok());
        const auto after_terminal = state.compose(
            PostDraft{"queue slot reused", {}, std::nullopt});
        CHECK(after_terminal.ok());
        CHECK_EQ(state.timeline().queued_count(), 16U);
        CHECK_EQ(state.timeline().size(), 17U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 17U);
        CHECK_EQ(random.call_count(), 18U);
        CHECK_EQ(clock.call_count(), 17U);
    });

    runner.run("small timelines reserve a slot for received posts", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(41);
        random.push_seed(42);
        random.push_seed(43);
        lora::test::FakeClock clock(700);
        MessengerState state(random, clock, 2);
        REQUIRE(state.initialize_new("alice").ok());
        REQUIRE(state.compose(PostDraft{"one", {}, std::nullopt}).ok());
        const auto rejected = state.compose(PostDraft{"two", {}, std::nullopt});
        CHECK_EQ(rejected.error, CommandError::QueueFull);
        CHECK_EQ(state.timeline().size(), 1U);
        CHECK_EQ(state.timeline().queued_count(), 1U);
        CHECK_EQ(state.timeline().last_assigned_order(), 1U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
        CHECK_EQ(random.call_count(), 2U);
        CHECK_EQ(random.remaining(), 1U);
        CHECK_EQ(clock.call_count(), 1U);

        const auto first_received = lora::test::make_post(43, 80, 1U, "received one");
        const auto second_received = lora::test::make_post(44, 80, 2U, "received two");
        REQUIRE(state.accept_received(first_received).ok());
        const auto next = state.accept_received(second_received);
        REQUIRE(next.ok());
        REQUIRE(next.evicted_message_id.has_value());
        CHECK_EQ(*next.evicted_message_id, first_received.message_id());
        CHECK_EQ(state.timeline().size(), 2U);
        CHECK_EQ(state.timeline().queued_count(), 1U);
        CHECK_EQ(state.timeline().last_assigned_order(), 3U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
        CHECK_EQ(random.call_count(), 2U);
        CHECK_EQ(random.remaining(), 1U);
        CHECK_EQ(clock.call_count(), 1U);
        CHECK(state.timeline().find(first_received.message_id()) == nullptr);
        CHECK(state.timeline().find(second_received.message_id()) != nullptr);
    });

    runner.run("timeline eviction is surfaced by compose without touching queued data", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(45);
        random.push_seed(46);
        lora::test::FakeClock clock(800);
        MessengerState state(random, clock, 2);
        REQUIRE(state.initialize_new("alice").ok());
        const auto oldest = lora::test::make_post(70, 71, 1U, "oldest");
        const auto newer = lora::test::make_post(72, 73, 1U, "newer");
        REQUIRE(state.accept_received(oldest).ok());
        REQUIRE(state.accept_received(newer).ok());

        const auto composed = state.compose(PostDraft{"local", {}, std::nullopt});
        REQUIRE(composed.ok());
        REQUIRE(composed.evicted_message_id.has_value());
        CHECK_EQ(*composed.evicted_message_id, oldest.message_id());
        CHECK(state.timeline().find(oldest.message_id()) == nullptr);
        CHECK(state.timeline().find(newer.message_id()) != nullptr);
        REQUIRE(composed.message_id.has_value());
        CHECK(state.timeline().find(*composed.message_id) != nullptr);
        CHECK_EQ(state.timeline().queued_count(), 1U);
        CHECK_EQ(state.timeline().last_assigned_order(), 3U);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
    });

    runner.run("sequence and timeline order exhaustion reject transactionally", [&] {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

        lora::test::ScriptedRandom sequence_random;
        sequence_random.push_seed(47);
        lora::test::FakeClock sequence_clock(900);
        MessengerState sequence_state(sequence_random, sequence_clock);
        REQUIRE(sequence_state.restore_identity(
            restored_identity(74, "alice", maximum)).ok());
        const auto sequence_result = sequence_state.compose(
            PostDraft{"cannot issue", {}, std::nullopt});
        CHECK_EQ(sequence_result.error, CommandError::SequenceExhausted);
        CHECK_EQ(sequence_random.call_count(), 0U);
        CHECK_EQ(sequence_clock.call_count(), 0U);
        CHECK(sequence_state.timeline().empty());
        CHECK_EQ(sequence_state.identity()->last_issued_sender_sequence(), maximum);

        lora::test::ScriptedRandom order_random;
        order_random.push_seed(48);
        order_random.push_seed(49);
        lora::test::FakeClock order_clock(901);
        MessengerState order_state(order_random, order_clock, 3, maximum);
        REQUIRE(order_state.initialize_new("alice").ok());
        const auto order_result = order_state.compose(
            PostDraft{"cannot order", {}, std::nullopt});
        CHECK_EQ(order_result.error, CommandError::OrderExhausted);
        CHECK_EQ(order_random.call_count(), 1U);
        CHECK_EQ(order_random.remaining(), 1U);
        CHECK_EQ(order_clock.call_count(), 0U);
        CHECK(order_state.timeline().empty());
        CHECK_EQ(order_state.timeline().last_assigned_order(), maximum);
        CHECK_EQ(order_state.identity()->last_issued_sender_sequence(), 0U);

        const auto received_result = order_state.accept_received(
            lora::test::make_post(49, 75, 1U, "cannot order either"));
        CHECK_EQ(received_result.error, CommandError::OrderExhausted);
        CHECK(order_state.timeline().empty());
        CHECK_EQ(order_random.call_count(), 1U);
        CHECK_EQ(order_clock.call_count(), 0U);
    });

    runner.run("received duplicates, conflicts, and own UUID retain received origin", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(50);
        lora::test::FakeClock clock(1'000);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        REQUIRE(state.identity().has_value());
        const auto own_seed = static_cast<std::uint8_t>(50);
        const auto received = lora::test::make_post(51, own_seed, 9U, "from wire");

        const auto inserted = state.accept_received(received);
        CHECK(inserted.ok());
        const auto* entry = state.timeline().find(received.message_id());
        REQUIRE(entry != nullptr);
        CHECK_EQ(entry->post.sender_id(), state.identity()->install_id());
        CHECK(std::holds_alternative<ReceivedOrigin>(entry->origin));
        CHECK_EQ(state.timeline().queued_count(), 0U);

        const auto duplicate = state.accept_received(received);
        CHECK_EQ(duplicate.error, CommandError::DuplicatePost);
        const auto conflict_post = lora::test::make_post(51, own_seed, 9U, "changed");
        const auto conflict = state.accept_received(conflict_post);
        CHECK_EQ(conflict.error, CommandError::ConflictingPost);
        CHECK_EQ(state.timeline().size(), 1U);
        CHECK_EQ(state.timeline().last_assigned_order(), 1U);
        REQUIRE(state.timeline().find(received.message_id()) != nullptr);
        CHECK_EQ(state.timeline().find(received.message_id())->post.body().value(), "from wire");
        CHECK_EQ(random.call_count(), 1U);
        CHECK_EQ(clock.call_count(), 0U);
    });

    runner.run("delivery command mapping preserves terminal state", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(52);
        random.push_seed(53);
        random.push_seed(54);
        lora::test::FakeClock clock(1'100);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        const auto local = state.compose(PostDraft{"local", {}, std::nullopt});
        REQUIRE(local.ok());
        REQUIRE(local.message_id.has_value());
        const auto failed_local = state.compose(
            PostDraft{"failed local", {}, std::nullopt});
        REQUIRE(failed_local.ok());
        REQUIRE(failed_local.message_id.has_value());
        const auto received = lora::test::make_post(55, 56);
        REQUIRE(state.accept_received(received).ok());

        CHECK(state.mark_broadcast(*local.message_id).ok());
        CHECK(state.mark_broadcast(*local.message_id).ok());
        CHECK_EQ(state.mark_failed(*local.message_id).error,
                 CommandError::InvalidTransition);
        const auto* local_entry = state.timeline().find(*local.message_id);
        REQUIRE(local_entry != nullptr);
        const auto* delivery = std::get_if<LocalDelivery>(&local_entry->origin);
        REQUIRE(delivery != nullptr);
        CHECK_EQ(delivery->state, LocalDeliveryState::Broadcast);

        CHECK(state.mark_failed(*failed_local.message_id).ok());
        CHECK(state.mark_failed(*failed_local.message_id).ok());
        CHECK_EQ(state.mark_broadcast(*failed_local.message_id).error,
                 CommandError::InvalidTransition);

        CHECK_EQ(state.mark_failed(received.message_id()).error,
                 CommandError::NotLocalPost);
        CHECK_EQ(state.mark_broadcast(lora::test::make_message_id(240)).error,
                 CommandError::MessageNotFound);
        CHECK_EQ(state.timeline().queued_count(), 0U);
        CHECK_EQ(random.call_count(), 3U);
        CHECK_EQ(clock.call_count(), 2U);
    });

    runner.run("local replies require a parent and store its strong ID", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(56);
        random.push_seed(57);
        random.push_seed(58);
        lora::test::FakeClock clock(1'200);
        MessengerState state(random, clock);
        REQUIRE(state.initialize_new("alice").ok());
        const auto parent = state.compose(PostDraft{"parent", {}, std::nullopt});
        REQUIRE(parent.ok());
        REQUIRE(parent.message_id.has_value());

        const auto reply = state.compose(PostDraft{"reply", {}, parent.message_id});
        REQUIRE(reply.ok());
        REQUIRE(reply.message_id.has_value());
        const auto* entry = state.timeline().find(*reply.message_id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->post.reply_to().has_value());
        CHECK_EQ(*entry->post.reply_to(), *parent.message_id);
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 2U);
    });

    runner.run("composing a local reply does not immediately evict its parent", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(59);
        random.push_seed(60);
        lora::test::FakeClock clock(1'300);
        MessengerState state(random, clock, 2);
        REQUIRE(state.initialize_new("alice").ok());
        const auto parent = lora::test::make_post(61, 70, 1U, "parent");
        const auto other = lora::test::make_post(62, 71, 1U, "other");
        REQUIRE(state.accept_received(parent).ok());
        REQUIRE(state.accept_received(other).ok());

        const auto reply = state.compose(
            PostDraft{"reply", {}, parent.message_id()});
        REQUIRE(reply.ok());
        REQUIRE(reply.message_id.has_value());
        REQUIRE(reply.evicted_message_id.has_value());
        CHECK_EQ(*reply.evicted_message_id, other.message_id());
        CHECK(state.timeline().find(parent.message_id()) != nullptr);
        CHECK(state.timeline().find(other.message_id()) == nullptr);
        const auto* reply_entry = state.timeline().find(*reply.message_id);
        REQUIRE(reply_entry != nullptr);
        CHECK_EQ(state.timeline().reply_state(*reply_entry),
                 lora::model::ReplyState::ParentAvailable);
        CHECK_EQ(random.call_count(), 2U);
        CHECK_EQ(clock.call_count(), 1U);
    });

    return runner.finish();
}
