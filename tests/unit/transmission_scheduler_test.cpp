/*
 * SPDX-License-Identifier: MIT
 */

#include "application/transmission_scheduler.h"

#include "protocol/fragmenter.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

class FillRandom final : public lora::ports::IRandomBytes {
public:
    explicit FillRandom(std::uint8_t value) : value_(value) {}

    bool fill(std::uint8_t* destination,
              std::size_t size) noexcept override {
        if (calls_ >= requested_sizes_.size()) {
            return false;
        }
        requested_sizes_[calls_] = size;
        ++calls_;
        if (fail_next_) {
            fail_next_ = false;
            return false;
        }
        std::fill(destination, destination + size, value_);
        return true;
    }

    void fail_next() noexcept { fail_next_ = true; }
    std::size_t calls() const noexcept { return calls_; }
    std::size_t requested_size(std::size_t index) const noexcept {
        return requested_sizes_[index];
    }

private:
    std::uint8_t value_;
    std::size_t calls_{0U};
    std::array<std::size_t, 64U> requested_sizes_{};
    bool fail_next_{false};
};

class ScriptedTransport final
    : public lora::ports::IDatagramTransport {
public:
    explicit ScriptedTransport(std::size_t mtu) : mtu_(mtu) {}

    std::size_t maximum_datagram_size() const noexcept override {
        return mtu_;
    }

    lora::ports::DatagramSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept override {
        offers_.emplace_back(data, data + size);
        if (statuses_.empty()) {
            return lora::ports::DatagramSendStatus::Accepted;
        }
        const auto status = statuses_.front();
        statuses_.pop_front();
        return status;
    }

    lora::ports::DatagramReceiveResult try_receive(
        std::uint8_t*, std::size_t) noexcept override {
        return {
            lora::ports::DatagramReceiveStatus::WouldBlock, 0U};
    }

    bool connected() const noexcept override {
        return connected_ && !closed_;
    }

    bool closed() const noexcept override { return closed_; }

    void close() noexcept override {
        closed_ = true;
        connected_ = false;
    }

    void push(lora::ports::DatagramSendStatus status) {
        statuses_.push_back(status);
    }

    void set_mtu(std::size_t mtu) noexcept { mtu_ = mtu; }

    const std::vector<lora::protocol::Bytes>& offers() const noexcept {
        return offers_;
    }

private:
    std::size_t mtu_;
    bool connected_{true};
    bool closed_{false};
    std::deque<lora::ports::DatagramSendStatus> statuses_;
    std::vector<lora::protocol::Bytes> offers_;
};

class ScriptedRadioPolicy final : public lora::ports::IRadioPolicy {
public:
    lora::ports::RadioTransmitDecision evaluate_transmit(
        lora::ports::RadioTick now,
        std::size_t datagram_size) noexcept override {
        evaluations_.push_back({now, datagram_size});
        if (decisions_.empty()) {
            return {lora::ports::RadioTransmitStatus::Allowed, now};
        }
        const auto decision = decisions_.front();
        decisions_.pop_front();
        return decision;
    }

    void record_transmit(
        lora::ports::RadioTick now,
        std::size_t datagram_size) noexcept override {
        records_.push_back({now, datagram_size});
    }

    void push(lora::ports::RadioTransmitStatus status,
              lora::ports::RadioTick not_before = 0U) {
        decisions_.push_back({status, not_before});
    }

    const std::vector<std::pair<lora::ports::RadioTick,
                                std::size_t>>&
    evaluations() const noexcept {
        return evaluations_;
    }

    const std::vector<std::pair<lora::ports::RadioTick,
                                std::size_t>>&
    records() const noexcept {
        return records_;
    }

private:
    std::deque<lora::ports::RadioTransmitDecision> decisions_;
    std::vector<std::pair<lora::ports::RadioTick, std::size_t>>
        evaluations_;
    std::vector<std::pair<lora::ports::RadioTick, std::size_t>>
        records_;
};

lora::model::PostPayload maximum_post() {
    auto input = lora::test::make_post_input(
        20U, 40U, 1U, std::string(160U, 'b'),
        std::string(24U, 'u'));
    input.mentions = {
        lora::test::make_uuid(100U),
        lora::test::make_uuid(110U),
        lora::test::make_uuid(120U),
        lora::test::make_uuid(130U),
    };
    input.reply_to = lora::test::make_uuid(19U);
    input.sender_time = std::numeric_limits<std::int64_t>::max();
    auto result =
        lora::model::PostPayload::create(std::move(input));
    return std::move(result).value();
}

} // namespace

int main() {
    using lora::application::OutboundTerminalState;
    using lora::application::SchedulerError;
    using lora::ports::DatagramSendStatus;
    using lora::ports::RadioTransmitStatus;
    lora::test::Runner runner;

    runner.run("enqueue validates MTU randomness duplicates and queue bound",
               [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto first =
            lora::test::make_post(10U, 30U, 1U, "first");

        CHECK_EQ(scheduler.enqueue(first, 47U, 0U),
                 SchedulerError::InvalidMtu);
        CHECK_EQ(scheduler.enqueue(first, 256U, 0U),
                 SchedulerError::InvalidMtu);
        CHECK_EQ(random.calls(), 0U);
        CHECK(scheduler.empty());

        random.fail_next();
        CHECK_EQ(scheduler.enqueue(first, 64U, 0U),
                 SchedulerError::RandomUnavailable);
        CHECK_EQ(random.calls(), 1U);
        CHECK(scheduler.empty());

        REQUIRE(scheduler.enqueue(first, 64U, 0U) ==
                SchedulerError::None);
        CHECK_EQ(scheduler.enqueue(first, 64U, 0U),
                 SchedulerError::DuplicateMessage);
        CHECK_EQ(random.calls(), 2U);
        for (std::size_t index = 1U;
             index < lora::application::kMaximumOutboundMessages;
             ++index) {
            const auto post = lora::test::make_post(
                static_cast<std::uint8_t>(10U + index), 30U,
                static_cast<std::uint64_t>(index + 1U), "queued");
            REQUIRE(scheduler.enqueue(post, 64U, 0U) ==
                    SchedulerError::None);
        }
        CHECK_EQ(scheduler.pending_count(),
                 lora::application::kMaximumOutboundMessages);
        CHECK_EQ(scheduler.metrics().pending_messages,
                 lora::application::kMaximumOutboundMessages);
        CHECK_EQ(scheduler.metrics().high_water_messages,
                 lora::application::kMaximumOutboundMessages);
        const auto overflow =
            lora::test::make_post(80U, 30U, 99U, "overflow");
        CHECK_EQ(scheduler.enqueue(overflow, 64U, 0U),
                 SchedulerError::QueueFull);

        scheduler.cancel_all();
        CHECK(scheduler.empty());
        CHECK_EQ(scheduler.metrics().pending_messages, 0U);
        CHECK_EQ(scheduler.metrics().high_water_messages,
                 lora::application::kMaximumOutboundMessages);
        CHECK_EQ(scheduler.metrics().failed_messages, 0U);
    });

    runner.run("stored jitter controls initial and repeat due ticks", [&] {
        FillRandom random(255U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(40U, 60U, 1U, "jitter");
        ScriptedTransport transport(128U);
        ScriptedRadioPolicy policy;

        REQUIRE(scheduler.enqueue(post, 128U, 100U) ==
                SchedulerError::None);
        REQUIRE(random.calls() == 1U);
        CHECK_EQ(random.requested_size(0U), 4U);
        CHECK(!scheduler.pump(349U, transport, policy).has_value());
        CHECK_EQ(policy.evaluations().size(), 0U);
        CHECK_EQ(transport.offers().size(), 0U);

        const auto broadcast =
            scheduler.pump(350U, transport, policy);
        REQUIRE(broadcast.has_value());
        CHECK_EQ(broadcast->message_id, post.message_id());
        CHECK_EQ(broadcast->state,
                 OutboundTerminalState::Broadcast);
        CHECK_EQ(scheduler.pending_count(), 1U);
        CHECK(!scheduler.pump(1'599U, transport, policy).has_value());
        CHECK_EQ(transport.offers().size(), 1U);
        CHECK(!scheduler.pump(1'600U, transport, policy).has_value());
        CHECK(scheduler.empty());
        CHECK_EQ(transport.offers().size(), 2U);
        CHECK_EQ(transport.offers()[0], transport.offers()[1]);
        CHECK_EQ(policy.records().size(), 2U);
        CHECK_EQ(policy.records()[0].first, 350U);
        CHECK_EQ(policy.records()[1].first, 1'600U);
        CHECK_EQ(scheduler.metrics().broadcast_messages, 1U);
        CHECK_EQ(scheduler.metrics().failed_messages, 0U);
    });

    runner.run("one pump offers one fragment and broadcasts after primary",
               [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post = maximum_post();
        const auto expected =
            lora::protocol::fragment_post(post, 48U);
        REQUIRE(expected.has_value());
        REQUIRE(expected.value().size() == 16U);
        ScriptedTransport transport(48U);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 48U, 0U) ==
                SchedulerError::None);

        for (std::size_t index = 0U;
             index + 1U < expected.value().size(); ++index) {
            CHECK(!scheduler.pump(0U, transport, policy).has_value());
            CHECK_EQ(transport.offers().size(), index + 1U);
        }
        const auto event = scheduler.pump(0U, transport, policy);
        REQUIRE(event.has_value());
        CHECK_EQ(event->state, OutboundTerminalState::Broadcast);
        CHECK_EQ(transport.offers(), expected.value());
        CHECK_EQ(policy.evaluations().size(), 16U);
        CHECK_EQ(policy.records().size(), 16U);
        CHECK_EQ(scheduler.metrics().transport_offers, 16U);
        CHECK_EQ(scheduler.metrics().accepted_datagrams, 16U);
        CHECK_EQ(scheduler.metrics().broadcast_messages, 1U);
        CHECK_EQ(scheduler.pending_count(), 1U);
    });

    runner.run("larger transport MTU is a logical-link mismatch", [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(54U, 74U, 12U, "capacity");
        ScriptedTransport transport(255U);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                SchedulerError::None);

        const auto event = scheduler.pump(0U, transport, policy);
        REQUIRE(event.has_value());
        CHECK_EQ(event->state, OutboundTerminalState::Failed);
        CHECK(scheduler.empty());
        CHECK_EQ(transport.offers().size(), 0U);
        CHECK_EQ(policy.evaluations().size(), 0U);
    });

    runner.run("backpressure uses bounded injected jitter before one retry",
               [&] {
        FillRandom random(255U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(41U, 61U, 2U, "retry");
        ScriptedTransport transport(128U);
        transport.push(DatagramSendStatus::WouldBlock);
        transport.push(DatagramSendStatus::Accepted);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                SchedulerError::None);

        CHECK(!scheduler.pump(249U, transport, policy).has_value());
        CHECK(!scheduler.pump(250U, transport, policy).has_value());
        CHECK(!scheduler.pump(449U, transport, policy).has_value());
        const auto event = scheduler.pump(450U, transport, policy);
        REQUIRE(event.has_value());
        CHECK_EQ(event->state, OutboundTerminalState::Broadcast);
        CHECK_EQ(transport.offers().size(), 2U);
        CHECK_EQ(policy.evaluations().size(), 2U);
        CHECK_EQ(policy.records().size(), 1U);
        CHECK_EQ(policy.records()[0].first, 450U);
        CHECK_EQ(scheduler.metrics().backpressure_events, 1U);
        CHECK_EQ(scheduler.metrics().transport_offers, 2U);
        CHECK_EQ(scheduler.metrics().accepted_datagrams, 1U);
    });

    runner.run("a second backpressure result exhausts the primary retry", [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(53U, 73U, 11U, "retry bound");
        ScriptedTransport transport(128U);
        transport.push(DatagramSendStatus::WouldBlock);
        transport.push(DatagramSendStatus::Disconnected);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                SchedulerError::None);

        CHECK(!scheduler.pump(0U, transport, policy).has_value());
        const auto failed =
            scheduler.pump(100U, transport, policy);
        REQUIRE(failed.has_value());
        CHECK_EQ(failed->message_id, post.message_id());
        CHECK_EQ(failed->state, OutboundTerminalState::Failed);
        CHECK(scheduler.empty());
        CHECK_EQ(transport.offers().size(), 2U);
        CHECK_EQ(policy.records().size(), 0U);
        CHECK_EQ(scheduler.metrics().backpressure_events, 2U);
        CHECK_EQ(scheduler.metrics().failed_messages, 1U);
    });

    runner.run("radio deferral gates transport and uses safe fallback delay",
               [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(42U, 62U, 3U, "policy");
        ScriptedTransport transport(128U);
        ScriptedRadioPolicy policy;
        policy.push(RadioTransmitStatus::Deferred, 0U);
        policy.push(RadioTransmitStatus::Deferred, 500U);
        policy.push(RadioTransmitStatus::Allowed);
        REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                SchedulerError::None);

        CHECK(!scheduler.pump(0U, transport, policy).has_value());
        CHECK(!scheduler.pump(99U, transport, policy).has_value());
        CHECK(!scheduler.pump(100U, transport, policy).has_value());
        CHECK(!scheduler.pump(499U, transport, policy).has_value());
        const auto event = scheduler.pump(500U, transport, policy);
        REQUIRE(event.has_value());
        CHECK_EQ(event->state, OutboundTerminalState::Broadcast);
        CHECK_EQ(policy.evaluations().size(), 3U);
        CHECK_EQ(transport.offers().size(), 1U);
        CHECK_EQ(policy.records().size(), 1U);
        CHECK_EQ(scheduler.metrics().policy_deferrals, 2U);
        CHECK_EQ(scheduler.metrics().transport_offers, 1U);
    });

    runner.run("locked invalid and terminal send states fail primary", [&] {
        {
            FillRandom random(0U);
            lora::application::TransmissionScheduler scheduler(random);
            const auto post =
                lora::test::make_post(43U, 63U, 4U, "locked");
            ScriptedTransport transport(128U);
            ScriptedRadioPolicy policy;
            policy.push(RadioTransmitStatus::Locked);
            REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                    SchedulerError::None);
            const auto event = scheduler.pump(0U, transport, policy);
            REQUIRE(event.has_value());
            CHECK_EQ(event->state, OutboundTerminalState::Failed);
            CHECK_EQ(transport.offers().size(), 0U);
            CHECK_EQ(policy.records().size(), 0U);
            CHECK_EQ(scheduler.metrics().failed_messages, 1U);
        }
        {
            FillRandom random(0U);
            lora::application::TransmissionScheduler scheduler(random);
            const auto post =
                lora::test::make_post(44U, 64U, 5U, "invalid");
            ScriptedTransport transport(128U);
            ScriptedRadioPolicy policy;
            policy.push(RadioTransmitStatus::Invalid);
            REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                    SchedulerError::None);
            const auto event = scheduler.pump(0U, transport, policy);
            REQUIRE(event.has_value());
            CHECK_EQ(event->state, OutboundTerminalState::Failed);
        }
        {
            FillRandom random(0U);
            lora::application::TransmissionScheduler scheduler(random);
            const auto post =
                lora::test::make_post(45U, 65U, 6U, "send invalid");
            ScriptedTransport transport(128U);
            transport.push(DatagramSendStatus::Invalid);
            ScriptedRadioPolicy policy;
            REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                    SchedulerError::None);
            const auto event = scheduler.pump(0U, transport, policy);
            REQUIRE(event.has_value());
            CHECK_EQ(event->state, OutboundTerminalState::Failed);
            CHECK_EQ(policy.records().size(), 0U);
        }
    });

    runner.run("deadline is fixed across retry and policy delays", [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(46U, 66U, 7U, "deadline");
        ScriptedTransport transport(128U);
        transport.push(DatagramSendStatus::WouldBlock);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 128U, 10U) ==
                SchedulerError::None);

        CHECK(!scheduler.pump(10U, transport, policy).has_value());
        CHECK(!scheduler.pump(109U, transport, policy).has_value());
        const auto failed =
            scheduler.pump(60'010U, transport, policy);
        REQUIRE(failed.has_value());
        CHECK_EQ(failed->message_id, post.message_id());
        CHECK_EQ(failed->state, OutboundTerminalState::Failed);
        CHECK(scheduler.empty());
        CHECK_EQ(transport.offers().size(), 1U);
        CHECK_EQ(scheduler.metrics().backpressure_events, 1U);
        CHECK_EQ(scheduler.metrics().failed_messages, 1U);
    });

    runner.run("transport contract drift fails before offering bytes", [&] {
        {
            FillRandom random(0U);
            lora::application::TransmissionScheduler scheduler(random);
            const auto post =
                lora::test::make_post(47U, 67U, 8U, "mtu drift");
            ScriptedTransport transport(128U);
            ScriptedRadioPolicy policy;
            REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                    SchedulerError::None);
            transport.set_mtu(64U);
            const auto event = scheduler.pump(0U, transport, policy);
            REQUIRE(event.has_value());
            CHECK_EQ(event->state, OutboundTerminalState::Failed);
            CHECK_EQ(transport.offers().size(), 0U);
            CHECK_EQ(policy.evaluations().size(), 0U);
        }
        {
            FillRandom random(0U);
            lora::application::TransmissionScheduler scheduler(random);
            const auto post =
                lora::test::make_post(48U, 68U, 9U, "closed");
            ScriptedTransport transport(128U);
            ScriptedRadioPolicy policy;
            REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                    SchedulerError::None);
            transport.close();
            const auto event = scheduler.pump(0U, transport, policy);
            REQUIRE(event.has_value());
            CHECK_EQ(event->state, OutboundTerminalState::Failed);
            CHECK_EQ(transport.offers().size(), 0U);
        }
    });

    runner.run("repeat pass is best effort after broadcast terminal event",
               [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(49U, 69U, 10U, "repeat");
        ScriptedTransport transport(128U);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(post, 128U, 0U) ==
                SchedulerError::None);

        const auto broadcast = scheduler.pump(0U, transport, policy);
        REQUIRE(broadcast.has_value());
        CHECK_EQ(broadcast->state,
                 OutboundTerminalState::Broadcast);
        transport.close();
        CHECK(!scheduler.pump(999U, transport, policy).has_value());
        CHECK_EQ(scheduler.pending_count(), 1U);
        CHECK(!scheduler.pump(1'000U, transport, policy).has_value());
        CHECK(scheduler.empty());
        CHECK_EQ(scheduler.metrics().broadcast_messages, 1U);
        CHECK_EQ(scheduler.metrics().failed_messages, 0U);
        CHECK_EQ(transport.offers().size(), 1U);
    });

    runner.run("multiple messages remain FIFO and cancellation is silent",
               [&] {
        FillRandom random(0U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto first =
            lora::test::make_post(50U, 70U, 1U, "first");
        const auto second =
            lora::test::make_post(51U, 70U, 2U, "second");
        ScriptedTransport transport(128U);
        ScriptedRadioPolicy policy;
        REQUIRE(scheduler.enqueue(first, 128U, 0U) ==
                SchedulerError::None);
        REQUIRE(scheduler.enqueue(second, 128U, 0U) ==
                SchedulerError::None);

        const auto first_event =
            scheduler.pump(0U, transport, policy);
        REQUIRE(first_event.has_value());
        CHECK_EQ(first_event->message_id, first.message_id());
        CHECK_EQ(scheduler.pending_count(), 2U);
        CHECK(!scheduler.pump(999U, transport, policy).has_value());
        CHECK(!scheduler.pump(1'000U, transport, policy).has_value());
        CHECK_EQ(scheduler.pending_count(), 1U);
        const auto second_event =
            scheduler.pump(1'000U, transport, policy);
        REQUIRE(second_event.has_value());
        CHECK_EQ(second_event->message_id, second.message_id());

        scheduler.cancel_all();
        CHECK(scheduler.empty());
        CHECK_EQ(scheduler.metrics().broadcast_messages, 2U);
        CHECK_EQ(scheduler.metrics().failed_messages, 0U);
    });

    runner.run("tick arithmetic saturates without offering after deadline",
               [&] {
        FillRandom random(255U);
        lora::application::TransmissionScheduler scheduler(random);
        const auto post =
            lora::test::make_post(52U, 72U, 3U, "saturate");
        ScriptedTransport transport(128U);
        ScriptedRadioPolicy policy;
        constexpr auto maximum =
            std::numeric_limits<lora::ports::RadioTick>::max();
        REQUIRE(scheduler.enqueue(post, 128U, maximum - 100U) ==
                SchedulerError::None);

        CHECK(!scheduler.pump(maximum - 1U, transport, policy)
                   .has_value());
        const auto event =
            scheduler.pump(maximum, transport, policy);
        REQUIRE(event.has_value());
        CHECK_EQ(event->state, OutboundTerminalState::Failed);
        CHECK_EQ(transport.offers().size(), 0U);
        CHECK_EQ(policy.evaluations().size(), 0U);
    });

    return runner.finish();
}
