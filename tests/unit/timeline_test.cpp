/*
 * SPDX-License-Identifier: MIT
 */

#include "model/timeline.h"

#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

int main() {
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::model::ReceivedOrigin;
    using lora::model::ReplyState;
    using lora::model::Timeline;
    using lora::model::TimelineInsertError;
    using lora::model::TransitionError;
    lora::test::Runner runner;

    runner.run("timeline capacity is bounded and nonzero", [&] {
        bool zero_rejected = false;
        try {
            Timeline invalid(0);
        } catch (const std::invalid_argument&) {
            zero_rejected = true;
        }
        CHECK(zero_rejected);

        bool over_maximum_rejected = false;
        try {
            Timeline invalid(257);
        } catch (const std::invalid_argument&) {
            over_maximum_rejected = true;
        }
        CHECK(over_maximum_rejected);

        Timeline maximum(256);
        CHECK_EQ(maximum.capacity(), 256U);
        CHECK(maximum.empty());

        Timeline below_maximum(255);
        CHECK_EQ(below_maximum.capacity(), 255U);
        CHECK_EQ(below_maximum.queued_capacity(), 16U);
    });

    runner.run("received and local inserts get monotonic local order", [&] {
        Timeline timeline(4);
        const auto own_install_id = lora::test::make_install_id(80);
        const auto received = lora::test::make_post(30, 80);
        const auto local = lora::test::make_post(31, 80, 2U);

        const auto received_result = timeline.insert_received(received);
        REQUIRE(received_result.ok());
        REQUIRE(received_result.received_order.has_value());
        CHECK_EQ(*received_result.received_order, 1U);
        const auto* received_entry = timeline.find(received.message_id());
        REQUIRE(received_entry != nullptr);
        CHECK(std::holds_alternative<ReceivedOrigin>(received_entry->origin));
        CHECK_EQ(received_entry->post.sender_id(), own_install_id);

        const auto local_result = timeline.insert_local(local);
        REQUIRE(local_result.ok());
        REQUIRE(local_result.received_order.has_value());
        CHECK_EQ(*local_result.received_order, 2U);
        CHECK_EQ(timeline.last_assigned_order(), 2U);
        CHECK_EQ(timeline.size(), 2U);
        CHECK_EQ(timeline.queued_count(), 1U);
        REQUIRE(timeline.newest_at(0) != nullptr);
        CHECK_EQ(timeline.newest_at(0)->post.message_id(), local.message_id());
        REQUIRE(timeline.newest_at(1) != nullptr);
        CHECK_EQ(timeline.newest_at(1)->post.message_id(), received.message_id());
        CHECK(timeline.newest_at(2) == nullptr);
    });

    runner.run("sender time extremes never affect local receive order", [&] {
        Timeline timeline(4);
        const auto future_hint = lora::test::make_post(
            60, 80, 1U, "future", {}, std::nullopt,
            std::numeric_limits<lora::ports::UnixSeconds>::max());
        const auto past_hint = lora::test::make_post(
            61, 80, 2U, "past", {}, std::nullopt,
            std::numeric_limits<lora::ports::UnixSeconds>::min());
        const auto equal_hint = lora::test::make_post(
            62, 80, 3U, "equal", {}, std::nullopt,
            std::numeric_limits<lora::ports::UnixSeconds>::max());

        REQUIRE(timeline.insert_received(future_hint).ok());
        REQUIRE(timeline.insert_received(past_hint).ok());
        REQUIRE(timeline.insert_received(equal_hint).ok());
        CHECK_EQ(timeline.find(future_hint.message_id())->received_order, 1U);
        CHECK_EQ(timeline.find(past_hint.message_id())->received_order, 2U);
        CHECK_EQ(timeline.find(equal_hint.message_id())->received_order, 3U);
        CHECK_EQ(timeline.newest_at(0)->post.message_id(), equal_hint.message_id());
        CHECK_EQ(timeline.newest_at(1)->post.message_id(), past_hint.message_id());
        CHECK_EQ(timeline.newest_at(2)->post.message_id(), future_hint.message_id());
    });

    runner.run("exact duplicates and ID conflicts do not mutate timeline state", [&] {
        Timeline timeline(3);
        const auto original = lora::test::make_post(32, 81, 1U, "original");
        REQUIRE(timeline.insert_received(original).ok());
        const auto size_before = timeline.size();
        const auto order_before = timeline.last_assigned_order();

        const auto duplicate = timeline.insert_received(original);
        CHECK_EQ(duplicate.error, TimelineInsertError::Duplicate);
        CHECK(!duplicate.received_order.has_value());
        CHECK(!duplicate.evicted_message_id.has_value());
        CHECK_EQ(timeline.size(), size_before);
        CHECK_EQ(timeline.last_assigned_order(), order_before);

        const auto conflict_post = lora::test::make_post(32, 81, 1U, "changed");
        const auto conflict = timeline.insert_received(conflict_post);
        CHECK_EQ(conflict.error, TimelineInsertError::Conflict);
        CHECK_EQ(timeline.size(), size_before);
        CHECK_EQ(timeline.last_assigned_order(), order_before);
        REQUIRE(timeline.find(original.message_id()) != nullptr);
        CHECK_EQ(timeline.find(original.message_id())->post.body().value(), "original");
    });

    runner.run("order exhaustion never wraps and duplicate detection takes precedence", [&] {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        Timeline timeline(3, maximum - 1U);
        const auto last = lora::test::make_post(33, 82);
        const auto inserted = timeline.insert_received(last);
        REQUIRE(inserted.ok());
        REQUIRE(inserted.received_order.has_value());
        CHECK_EQ(*inserted.received_order, maximum);
        CHECK_EQ(timeline.last_assigned_order(), maximum);

        const auto duplicate = timeline.insert_received(last);
        CHECK_EQ(duplicate.error, TimelineInsertError::Duplicate);
        const auto exhausted = timeline.insert_received(lora::test::make_post(34, 82, 2U));
        CHECK_EQ(exhausted.error, TimelineInsertError::OrderExhausted);
        CHECK_EQ(timeline.size(), 1U);
        CHECK_EQ(timeline.last_assigned_order(), maximum);
    });

    runner.run("queued local capacity is enforced by the timeline aggregate", [&] {
        Timeline timeline(32);
        CHECK_EQ(timeline.queued_capacity(), 16U);
        std::vector<lora::model::PostPayload> queued;
        for (std::uint8_t index = 0; index < 16; ++index) {
            queued.push_back(lora::test::make_post(
                static_cast<std::uint8_t>(100 + index), 83,
                static_cast<std::uint64_t>(index) + 1U));
            REQUIRE(timeline.insert_local(queued.back()).ok());
        }
        CHECK_EQ(timeline.queued_count(), 16U);
        CHECK_EQ(timeline.size(), 16U);
        CHECK_EQ(timeline.last_assigned_order(), 16U);

        const auto duplicate = timeline.insert_local(queued.front());
        CHECK_EQ(duplicate.error, TimelineInsertError::Duplicate);
        const auto rejected = timeline.insert_local(
            lora::test::make_post(116, 83, 17U));
        CHECK_EQ(rejected.error, TimelineInsertError::QueueFull);
        CHECK_EQ(timeline.queued_count(), 16U);
        CHECK_EQ(timeline.size(), 16U);
        CHECK_EQ(timeline.last_assigned_order(), 16U);

        Timeline receive_only(1);
        CHECK_EQ(receive_only.queued_capacity(), 0U);
        const auto no_queue = receive_only.insert_local(
            lora::test::make_post(117, 83, 1U));
        CHECK_EQ(no_queue.error, TimelineInsertError::QueueFull);
        REQUIRE(receive_only.insert_received(
            lora::test::make_post(118, 84, 1U)).ok());
    });

    runner.run("insertion evicts the oldest non-queued entry and reports its ID", [&] {
        Timeline timeline(3);
        const auto queued = lora::test::make_post(38, 85, 1U);
        const auto oldest_received = lora::test::make_post(39, 86, 1U);
        const auto newer_received = lora::test::make_post(40, 87, 1U);
        REQUIRE(timeline.insert_local(queued).ok());
        REQUIRE(timeline.insert_received(oldest_received).ok());
        REQUIRE(timeline.insert_received(newer_received).ok());

        const auto newest = lora::test::make_post(41, 88, 1U);
        const auto inserted = timeline.insert_received(newest);
        REQUIRE(inserted.ok());
        REQUIRE(inserted.evicted_message_id.has_value());
        CHECK_EQ(*inserted.evicted_message_id, oldest_received.message_id());
        CHECK_EQ(timeline.size(), 3U);
        CHECK_EQ(timeline.last_assigned_order(), 4U);
        CHECK(timeline.find(queued.message_id()) != nullptr);
        CHECK(timeline.find(oldest_received.message_id()) == nullptr);
        CHECK(timeline.find(newer_received.message_id()) != nullptr);
        CHECK(timeline.find(newest.message_id()) != nullptr);

        const auto replay_after_retention = timeline.insert_received(oldest_received);
        REQUIRE(replay_after_retention.ok());
        REQUIRE(replay_after_retention.evicted_message_id.has_value());
        CHECK_EQ(*replay_after_retention.evicted_message_id,
                 newer_received.message_id());
        CHECK_EQ(timeline.last_assigned_order(), 5U);
        CHECK(timeline.find(oldest_received.message_id()) != nullptr);

        CHECK_EQ(timeline.mark_failed(queued.message_id()), TransitionError::None);
        const auto after_terminal = lora::test::make_post(42, 89, 1U);
        const auto terminal_eviction = timeline.insert_received(after_terminal);
        REQUIRE(terminal_eviction.ok());
        REQUIRE(terminal_eviction.evicted_message_id.has_value());
        CHECK_EQ(*terminal_eviction.evicted_message_id, queued.message_id());
        CHECK(timeline.find(queued.message_id()) == nullptr);
    });

    runner.run("a local reply preserves its parent during the same insertion", [&] {
        Timeline timeline(2);
        const auto parent = lora::test::make_post(63, 90, 1U, "parent");
        const auto other = lora::test::make_post(64, 91, 1U, "other");
        const auto reply = lora::test::make_post(
            65, 92, 1U, "reply", {}, parent.message_id());
        REQUIRE(timeline.insert_received(parent).ok());
        REQUIRE(timeline.insert_received(other).ok());

        const auto inserted = timeline.insert_local(reply);
        REQUIRE(inserted.ok());
        REQUIRE(inserted.evicted_message_id.has_value());
        CHECK_EQ(*inserted.evicted_message_id, other.message_id());
        CHECK(timeline.find(parent.message_id()) != nullptr);
        CHECK(timeline.find(other.message_id()) == nullptr);
        const auto* reply_entry = timeline.find(reply.message_id());
        REQUIRE(reply_entry != nullptr);
        CHECK_EQ(timeline.reply_state(*reply_entry), ReplyState::ParentAvailable);
    });

    runner.run("local delivery transitions are terminal and idempotent", [&] {
        Timeline timeline(5);
        const auto broadcast = lora::test::make_post(43, 90, 1U);
        const auto failed = lora::test::make_post(44, 90, 2U);
        const auto received = lora::test::make_post(45, 91, 1U);
        REQUIRE(timeline.insert_local(broadcast).ok());
        REQUIRE(timeline.insert_local(failed).ok());
        REQUIRE(timeline.insert_received(received).ok());

        CHECK_EQ(timeline.mark_broadcast(broadcast.message_id()), TransitionError::None);
        CHECK_EQ(timeline.mark_broadcast(broadcast.message_id()), TransitionError::None);
        CHECK_EQ(timeline.mark_failed(broadcast.message_id()), TransitionError::InvalidState);
        const auto* broadcast_entry = timeline.find(broadcast.message_id());
        REQUIRE(broadcast_entry != nullptr);
        const auto* broadcast_local = std::get_if<LocalDelivery>(&broadcast_entry->origin);
        REQUIRE(broadcast_local != nullptr);
        CHECK_EQ(broadcast_local->state, LocalDeliveryState::Broadcast);

        CHECK_EQ(timeline.mark_failed(failed.message_id()), TransitionError::None);
        CHECK_EQ(timeline.mark_failed(failed.message_id()), TransitionError::None);
        CHECK_EQ(timeline.mark_broadcast(failed.message_id()), TransitionError::InvalidState);
        CHECK_EQ(timeline.queued_count(), 0U);

        CHECK_EQ(timeline.mark_failed(received.message_id()), TransitionError::NotLocal);
        CHECK_EQ(timeline.mark_broadcast(lora::test::make_message_id(250)),
                 TransitionError::NotFound);
    });

    runner.run("unknown restored delivery state cannot be rewritten", [&] {
        Timeline timeline(2);
        const auto post = lora::test::make_post(46, 92);
        REQUIRE(timeline.insert_local(post, LocalDeliveryState::Unknown).ok());
        const auto* entry = timeline.find(post.message_id());
        REQUIRE(entry != nullptr);
        const auto* local = std::get_if<LocalDelivery>(&entry->origin);
        REQUIRE(local != nullptr);
        CHECK_EQ(timeline.mark_broadcast(post.message_id()), TransitionError::InvalidState);
        CHECK_EQ(timeline.mark_failed(post.message_id()), TransitionError::InvalidState);
        CHECK_EQ(local->state, LocalDeliveryState::Unknown);
    });

    runner.run("reply and mention helpers reflect current local history", [&] {
        Timeline timeline(4);
        const auto me = lora::test::make_install_id(93);
        const auto parent = lora::test::make_post(47, 94);
        const auto plain = lora::test::make_post(48, 94, 2U);
        const auto reply = lora::test::make_post(
            49, 94, 3U, "reply", {me}, parent.message_id());
        const auto orphan = lora::test::make_post(
            50, 94, 4U, "orphan", {}, lora::test::make_message_id(200));
        REQUIRE(timeline.insert_received(parent).ok());
        REQUIRE(timeline.insert_received(plain).ok());
        REQUIRE(timeline.insert_received(reply).ok());
        REQUIRE(timeline.insert_received(orphan).ok());

        REQUIRE(timeline.find(plain.message_id()) != nullptr);
        REQUIRE(timeline.find(reply.message_id()) != nullptr);
        REQUIRE(timeline.find(orphan.message_id()) != nullptr);
        CHECK_EQ(timeline.reply_state(*timeline.find(plain.message_id())), ReplyState::NotReply);
        CHECK_EQ(timeline.reply_state(*timeline.find(reply.message_id())), ReplyState::ParentAvailable);
        CHECK_EQ(timeline.reply_state(*timeline.find(orphan.message_id())), ReplyState::ParentUnavailable);
        CHECK(timeline.mentions(*timeline.find(reply.message_id()), me));
        CHECK(!timeline.mentions(*timeline.find(plain.message_id()), me));

        const auto late_parent = lora::test::make_post(200, 95, 1U, "late parent");
        REQUIRE(timeline.insert_received(late_parent).ok());
        REQUIRE(timeline.find(orphan.message_id()) != nullptr);
        CHECK_EQ(timeline.reply_state(*timeline.find(orphan.message_id())),
                 ReplyState::ParentAvailable);
    });

    return runner.finish();
}
