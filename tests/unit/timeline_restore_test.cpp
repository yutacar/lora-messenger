/*
 * SPDX-License-Identifier: MIT
 */

#include "model/timeline.h"

#include "test_model_helpers.h"
#include "test_support.h"

#include <cstdint>
#include <utility>
#include <vector>

int main() {
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::model::ReceivedOrigin;
    using lora::model::Timeline;
    using lora::model::TimelineEntry;
    using lora::model::TimelineRestoreError;
    lora::test::Runner runner;

    runner.run("restore preserves sparse order and origins", [&] {
        std::vector<TimelineEntry> entries;
        entries.push_back(
            {lora::test::make_post(10, 40), 3U, ReceivedOrigin{}});
        entries.push_back(
            {lora::test::make_post(11, 41), 7U,
             LocalDelivery{LocalDeliveryState::Unknown}});

        auto restored = Timeline::restore(8U, 9U, std::move(entries));
        REQUIRE(restored.has_value());
        CHECK_EQ(restored.value().capacity(), 8U);
        CHECK_EQ(restored.value().size(), 2U);
        CHECK_EQ(restored.value().last_assigned_order(), 9U);
        REQUIRE(restored.value().newest_at(0) != nullptr);
        CHECK_EQ(restored.value().newest_at(0)->received_order, 7U);
        const auto* local = std::get_if<LocalDelivery>(
            &restored.value().newest_at(0)->origin);
        REQUIRE(local != nullptr);
        CHECK_EQ(local->state, LocalDeliveryState::Unknown);
    });

    runner.run("restore rejects invalid capacities and excess rows", [&] {
        auto zero = Timeline::restore(0U, 0U, {});
        CHECK(!zero.has_value());
        CHECK_EQ(zero.error(), TimelineRestoreError::InvalidCapacity);

        auto over = Timeline::restore(257U, 0U, {});
        CHECK(!over.has_value());
        CHECK_EQ(over.error(), TimelineRestoreError::InvalidCapacity);

        std::vector<TimelineEntry> entries;
        entries.push_back(
            {lora::test::make_post(20, 50), 1U, ReceivedOrigin{}});
        entries.push_back(
            {lora::test::make_post(21, 50), 2U, ReceivedOrigin{}});
        auto too_many = Timeline::restore(1U, 2U, std::move(entries));
        CHECK(!too_many.has_value());
        CHECK_EQ(too_many.error(), TimelineRestoreError::TooManyEntries);
    });

    runner.run("restore rejects zero descending and future orders", [&] {
        auto zero_order = Timeline::restore(
            2U, 2U,
            {{lora::test::make_post(30, 60), 0U, ReceivedOrigin{}}});
        CHECK(!zero_order.has_value());
        CHECK_EQ(zero_order.error(), TimelineRestoreError::InvalidOrder);

        auto descending = Timeline::restore(
            3U, 3U,
            {{lora::test::make_post(31, 60), 2U, ReceivedOrigin{}},
             {lora::test::make_post(32, 60), 1U, ReceivedOrigin{}}});
        CHECK(!descending.has_value());
        CHECK_EQ(descending.error(), TimelineRestoreError::InvalidOrder);

        auto future = Timeline::restore(
            2U, 4U,
            {{lora::test::make_post(33, 60), 5U, ReceivedOrigin{}}});
        CHECK(!future.has_value());
        CHECK_EQ(future.error(), TimelineRestoreError::InvalidOrder);
    });

    runner.run("restore rejects duplicate IDs and excess queued rows", [&] {
        const auto duplicate = lora::test::make_post(40, 70);
        auto duplicate_result = Timeline::restore(
            3U, 2U,
            {{duplicate, 1U, ReceivedOrigin{}},
             {duplicate, 2U, ReceivedOrigin{}}});
        CHECK(!duplicate_result.has_value());
        CHECK_EQ(duplicate_result.error(),
                 TimelineRestoreError::DuplicateMessageId);

        std::vector<TimelineEntry> queued;
        for (std::uint8_t index = 0; index < 17U; ++index) {
            queued.push_back(
                {lora::test::make_post(
                     static_cast<std::uint8_t>(50U + index), 80,
                     static_cast<std::uint64_t>(index) + 1U),
                 static_cast<std::uint64_t>(index) + 1U,
                 LocalDelivery{LocalDeliveryState::Queued}});
        }
        auto excess = Timeline::restore(18U, 17U, std::move(queued));
        CHECK(!excess.has_value());
        CHECK_EQ(excess.error(),
                 TimelineRestoreError::TooManyQueuedEntries);
    });

    return runner.finish();
}
