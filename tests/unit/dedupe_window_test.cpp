/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/dedupe_window.h"

#include "protocol/post_codec.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

lora::protocol::Bytes encode(
    const lora::model::PostPayload& post) {
    auto result = lora::protocol::encode_post(post);
    return std::move(result).value();
}

lora::protocol::SeenMessageRecord record(
    const lora::model::PostPayload& post,
    std::uint64_t seen_order) {
    return {post.message_id(), seen_order, encode(post)};
}

} // namespace

int main() {
    using lora::protocol::DedupeClassification;
    using lora::protocol::DedupeError;
    using lora::protocol::MessageDedupeWindow;
    lora::test::Runner runner;

    runner.run("new identical and conflicting posts are distinct states", [&] {
        MessageDedupeWindow window(3U);
        const auto original =
            lora::test::make_post(10U, 40U, 7U, "original");
        const auto identical = encode(original);
        const auto conflicting =
            lora::test::make_post(10U, 40U, 7U, "changed");
        const auto conflicting_bytes = encode(conflicting);

        CHECK_EQ(window.capacity(), 3U);
        CHECK(window.empty());
        CHECK_EQ(window.classify(original.message_id(), identical),
                 DedupeClassification::New);
        CHECK_EQ(window.remember(original, identical),
                 DedupeError::None);
        CHECK_EQ(window.size(), 1U);
        CHECK_EQ(window.last_seen_order(), 1U);
        CHECK_EQ(window.classify(original.message_id(), identical),
                 DedupeClassification::Duplicate);
        CHECK_EQ(window.classify(original.message_id(),
                                 conflicting_bytes),
                 DedupeClassification::Conflict);

        CHECK_EQ(window.remember(original, identical),
                 DedupeError::None);
        CHECK_EQ(window.size(), 1U);
        CHECK_EQ(window.last_seen_order(), 1U);
        CHECK_EQ(window.remember(conflicting, conflicting_bytes),
                 DedupeError::DuplicateMessageId);
        CHECK_EQ(window.records().front().encoded_post, identical);
    });

    runner.run("remember rejects noncanonical and mismatched bytes atomically",
               [&] {
        MessageDedupeWindow window(2U);
        const auto first =
            lora::test::make_post(11U, 41U, 1U, "first");
        const auto second =
            lora::test::make_post(12U, 41U, 2U, "second");
        auto malformed = encode(first);
        malformed.push_back(0U);

        CHECK_EQ(window.remember(first, malformed),
                 DedupeError::InvalidRecord);
        CHECK(window.empty());
        CHECK_EQ(window.last_seen_order(), 0U);
        CHECK_EQ(window.remember(first, encode(second)),
                 DedupeError::InvalidRecord);
        CHECK(window.empty());
        CHECK_EQ(window.last_seen_order(), 0U);
    });

    runner.run("capacity evicts the oldest unprotected record", [&] {
        MessageDedupeWindow window(2U);
        const auto first =
            lora::test::make_post(20U, 50U, 1U, "one");
        const auto second =
            lora::test::make_post(21U, 50U, 2U, "two");
        const auto third =
            lora::test::make_post(22U, 50U, 3U, "three");
        const auto fourth =
            lora::test::make_post(23U, 50U, 4U, "four");

        REQUIRE(window.remember(first, encode(first)) ==
                DedupeError::None);
        REQUIRE(window.remember(second, encode(second)) ==
                DedupeError::None);
        REQUIRE(window.remember(
                    third, encode(third), {first.message_id()}) ==
                DedupeError::None);

        CHECK_EQ(window.size(), 2U);
        CHECK_EQ(window.last_seen_order(), 3U);
        CHECK_EQ(window.classify(first.message_id(), encode(first)),
                 DedupeClassification::Duplicate);
        CHECK_EQ(window.classify(second.message_id(), encode(second)),
                 DedupeClassification::New);
        CHECK_EQ(window.classify(third.message_id(), encode(third)),
                 DedupeClassification::Duplicate);
        REQUIRE(window.records().size() == 2U);
        CHECK_EQ(window.records()[0].message_id, first.message_id());
        CHECK_EQ(window.records()[0].seen_order, 1U);
        CHECK_EQ(window.records()[1].message_id, third.message_id());
        CHECK_EQ(window.records()[1].seen_order, 3U);

        CHECK_EQ(window.remember(
                     fourth, encode(fourth),
                     {first.message_id(), third.message_id()}),
                 DedupeError::ProtectedCapacity);
        CHECK_EQ(window.size(), 2U);
        CHECK_EQ(window.last_seen_order(), 3U);
        CHECK_EQ(window.classify(fourth.message_id(), encode(fourth)),
                 DedupeClassification::New);
    });

    runner.run("restore accepts ordered bounded canonical history", [&] {
        const auto first =
            lora::test::make_post(30U, 60U, 1U, "one");
        const auto second =
            lora::test::make_post(31U, 60U, 2U, "two");
        std::vector<lora::protocol::SeenMessageRecord> records{
            record(first, 2U), record(second, 5U)};

        auto restored =
            MessageDedupeWindow::restore(3U, 5U, records);
        REQUIRE(restored.has_value());
        CHECK_EQ(restored.value().capacity(), 3U);
        CHECK_EQ(restored.value().size(), 2U);
        CHECK_EQ(restored.value().last_seen_order(), 5U);
        REQUIRE(restored.value().records().size() == records.size());
        for (std::size_t index = 0U; index < records.size(); ++index) {
            CHECK_EQ(restored.value().records()[index].message_id,
                     records[index].message_id);
            CHECK_EQ(restored.value().records()[index].seen_order,
                     records[index].seen_order);
            CHECK_EQ(restored.value().records()[index].encoded_post,
                     records[index].encoded_post);
        }

        const auto third =
            lora::test::make_post(32U, 60U, 3U, "three");
        REQUIRE(restored.value().remember(third, encode(third)) ==
                DedupeError::None);
        CHECK_EQ(restored.value().last_seen_order(), 6U);
        CHECK_EQ(restored.value().records().back().seen_order, 6U);
    });

    runner.run("restore fails closed on capacity and record corruption", [&] {
        const auto first =
            lora::test::make_post(40U, 70U, 1U, "one");
        const auto second =
            lora::test::make_post(41U, 70U, 2U, "two");

        CHECK_EQ(MessageDedupeWindow::restore(0U, 0U, {}).error(),
                 DedupeError::InvalidCapacity);
        CHECK_EQ(MessageDedupeWindow::restore(
                     1U, 2U,
                     {record(first, 1U), record(second, 2U)})
                     .error(),
                 DedupeError::TooManyRecords);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 2U, {record(first, 1U)})
                     .error(),
                 DedupeError::InvalidOrder);

        auto malformed = record(first, 1U);
        malformed.encoded_post.push_back(0U);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 1U, {malformed})
                     .error(),
                 DedupeError::InvalidRecord);

        auto mismatched = record(first, 1U);
        mismatched.message_id = second.message_id();
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 1U, {mismatched})
                     .error(),
                 DedupeError::InvalidRecord);

        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 2U,
                     {record(first, 1U), record(first, 2U)})
                     .error(),
                 DedupeError::DuplicateMessageId);
    });

    runner.run("restore rejects every invalid order relation", [&] {
        const auto first =
            lora::test::make_post(50U, 80U, 1U, "one");
        const auto second =
            lora::test::make_post(51U, 80U, 2U, "two");

        CHECK_EQ(MessageDedupeWindow::restore(2U, 1U, {}).error(),
                 DedupeError::InvalidOrder);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 1U, {record(first, 0U)})
                     .error(),
                 DedupeError::InvalidRecord);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 1U, {record(first, 2U)})
                     .error(),
                 DedupeError::InvalidOrder);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 2U,
                     {record(first, 2U), record(second, 1U)})
                     .error(),
                 DedupeError::InvalidOrder);
        CHECK_EQ(MessageDedupeWindow::restore(
                     2U, 1U,
                     {record(first, 1U), record(second, 1U)})
                     .error(),
                 DedupeError::InvalidOrder);
    });

    runner.run("order exhaustion cannot mutate restored state", [&] {
        const auto first =
            lora::test::make_post(60U, 90U, 1U, "one");
        const auto second =
            lora::test::make_post(61U, 90U, 2U, "two");
        constexpr auto maximum =
            std::numeric_limits<std::uint64_t>::max();
        auto restored = MessageDedupeWindow::restore(
            2U, maximum, {record(first, maximum)});
        REQUIRE(restored.has_value());

        CHECK_EQ(restored.value().remember(second, encode(second)),
                 DedupeError::OrderExhausted);
        CHECK_EQ(restored.value().size(), 1U);
        CHECK_EQ(restored.value().last_seen_order(), maximum);
        CHECK_EQ(restored.value().remember(first, encode(first)),
                 DedupeError::None);
        CHECK_EQ(restored.value().size(), 1U);
        CHECK_EQ(restored.value().last_seen_order(), maximum);
    });

    return runner.finish();
}
