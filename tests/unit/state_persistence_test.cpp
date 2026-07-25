/*
 * SPDX-License-Identifier: MIT
 */

#include "application/messenger_state.h"

#include "test_fakes.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

lora::model::Identity identity(std::uint8_t discriminator,
                               std::uint64_t sequence = 0) {
    auto user_id = lora::core::UserId::create("Mika");
    return lora::model::Identity::restore(
        lora::test::make_install_id(discriminator),
        std::move(user_id).value(), sequence);
}

class RecordingCommit final : public lora::ports::IStateCommit {
public:
    bool persist_identity(const lora::model::Identity& value) noexcept override {
        ++identity_calls;
        last_sequence = value.last_issued_sender_sequence();
        return identity_succeeds;
    }

    bool persist_timeline(const lora::model::Timeline& value) noexcept override {
        ++timeline_calls;
        last_timeline_size = value.size();
        return timeline_succeeds;
    }

    bool identity_succeeds{true};
    bool timeline_succeeds{true};
    std::size_t identity_calls{0};
    std::size_t timeline_calls{0};
    std::uint64_t last_sequence{0};
    std::size_t last_timeline_size{0};
};

} // namespace

int main() {
    using lora::application::CommandError;
    using lora::application::MessengerState;
    using lora::model::LocalDelivery;
    using lora::model::LocalDeliveryState;
    using lora::model::PostDraft;
    lora::test::Runner runner;

    runner.run("failed identity persistence leaves initialize and rename unchanged", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(10);
        lora::test::FakeClock clock;
        RecordingCommit commit;
        commit.identity_succeeds = false;
        MessengerState state(random, clock, 8U, 0U, &commit);

        CHECK_EQ(state.initialize_new("Mika").error,
                 CommandError::PersistenceUnavailable);
        CHECK(!state.identity().has_value());
        CHECK_EQ(commit.identity_calls, 1U);

        REQUIRE(state.restore_identity(identity(11)).ok());
        CHECK_EQ(state.rename_user("Renamed").error,
                 CommandError::PersistenceUnavailable);
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->user_id().value(), "Mika");
    });

    runner.run("compose reserves sequence before timeline persistence", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(20);
        random.push_seed(21);
        lora::test::FakeClock clock(123);
        RecordingCommit commit;
        MessengerState state(random, clock, 8U, 0U, &commit);
        REQUIRE(state.restore_identity(identity(12)).ok());

        commit.timeline_succeeds = false;
        const auto failed = state.compose(
            PostDraft{"first", {}, std::nullopt});
        CHECK_EQ(failed.error, CommandError::PersistenceUnavailable);
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 1U);
        CHECK(state.timeline().empty());
        CHECK_EQ(commit.last_sequence, 1U);
        CHECK_EQ(commit.last_timeline_size, 1U);

        commit.timeline_succeeds = true;
        const auto next = state.compose(
            PostDraft{"second", {}, std::nullopt});
        REQUIRE(next.ok());
        REQUIRE(next.message_id.has_value());
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 2U);
        CHECK_EQ(state.timeline().size(), 1U);
        const auto* entry = state.timeline().find(*next.message_id);
        REQUIRE(entry != nullptr);
        CHECK_EQ(entry->post.sender_sequence(), 2U);
    });

    runner.run("identity reservation failure commits neither identity nor timeline", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(30);
        lora::test::FakeClock clock(456);
        RecordingCommit commit;
        commit.identity_succeeds = false;
        MessengerState state(random, clock, 8U, 0U, &commit);
        REQUIRE(state.restore_identity(identity(13)).ok());

        CHECK_EQ(state.compose(PostDraft{"body", {}, std::nullopt}).error,
                 CommandError::PersistenceUnavailable);
        REQUIRE(state.identity().has_value());
        CHECK_EQ(state.identity()->last_issued_sender_sequence(), 0U);
        CHECK(state.timeline().empty());
        CHECK_EQ(commit.timeline_calls, 0U);
    });

    runner.run("receive and transition persistence failures preserve live timeline", [&] {
        lora::test::ScriptedRandom random;
        random.push_seed(40);
        lora::test::FakeClock clock;
        RecordingCommit commit;
        MessengerState state(random, clock, 8U, 0U, &commit);
        REQUIRE(state.restore_identity(identity(14)).ok());

        const auto received = lora::test::make_post(41, 90);
        commit.timeline_succeeds = false;
        CHECK_EQ(state.accept_received(received).error,
                 CommandError::PersistenceUnavailable);
        CHECK(state.timeline().empty());

        commit.timeline_succeeds = true;
        const auto local = state.compose(PostDraft{"local", {}, std::nullopt});
        REQUIRE(local.ok());
        REQUIRE(local.message_id.has_value());
        commit.timeline_succeeds = false;
        CHECK_EQ(state.mark_broadcast(*local.message_id).error,
                 CommandError::PersistenceUnavailable);
        const auto* entry = state.timeline().find(*local.message_id);
        REQUIRE(entry != nullptr);
        const auto* delivery = std::get_if<LocalDelivery>(&entry->origin);
        REQUIRE(delivery != nullptr);
        CHECK_EQ(delivery->state, LocalDeliveryState::Queued);
    });

    runner.run("restore_state adopts exact validated history without persistence", [&] {
        lora::test::ScriptedRandom random;
        lora::test::FakeClock clock;
        RecordingCommit commit;
        MessengerState state(random, clock, 8U, 0U, &commit);
        auto restored_timeline = lora::model::Timeline::restore(
            8U, 9U,
            {{lora::test::make_post(50, 91), 7U,
              lora::model::ReceivedOrigin{}}});
        REQUIRE(restored_timeline.has_value());
        REQUIRE(state.restore_state(identity(15, 6U),
                                    std::move(restored_timeline).value()).ok());
        CHECK_EQ(state.timeline().last_assigned_order(), 9U);
        CHECK_EQ(state.timeline().size(), 1U);
        CHECK_EQ(commit.identity_calls, 0U);
        CHECK_EQ(commit.timeline_calls, 0U);
    });

    return runner.finish();
}
