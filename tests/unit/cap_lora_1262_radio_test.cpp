/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/radio/cap_lora_1262_radio.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace {

using lora::adapters::radio::ICapLora1262Radio;
using lora::adapters::radio::RadioPollResult;
using lora::adapters::radio::RadioPollStatus;
using lora::adapters::radio::RadioStartStatus;

class FakeCapRadio final : public ICapLora1262Radio {
public:
    bool ready() const noexcept override {
        return ready_;
    }

    RadioStartStatus try_start_transmit(
        const std::uint8_t* data, std::size_t size) noexcept override {
        ++start_calls_;
        if (start_status_ == RadioStartStatus::Started) {
            transmitted_.assign(data, data + size);
        }
        return start_status_;
    }

    RadioPollResult poll(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override {
        ++poll_calls_;
        if (poll_results_.empty()) {
            return {};
        }
        const auto scripted = poll_results_.front();
        poll_results_.pop_front();
        if (scripted.status == RadioPollStatus::Received &&
            scripted.bytes.size() <= destination_capacity) {
            std::copy(
                scripted.bytes.begin(), scripted.bytes.end(),
                destination);
            return {scripted.status, scripted.bytes.size()};
        }
        return {scripted.status, scripted.reported_size};
    }

    void shutdown() noexcept override {
        ++shutdown_calls_;
        ready_ = false;
    }

    void push_poll(
        RadioPollStatus status,
        std::vector<std::uint8_t> bytes = {},
        std::size_t reported_size = 0U) {
        poll_results_.push_back(
            ScriptedPoll{status, std::move(bytes), reported_size});
    }

    struct ScriptedPoll {
        RadioPollStatus status{RadioPollStatus::Idle};
        std::vector<std::uint8_t> bytes;
        std::size_t reported_size{0U};
    };

    bool ready_{true};
    RadioStartStatus start_status_{RadioStartStatus::Started};
    std::deque<ScriptedPoll> poll_results_;
    std::vector<std::uint8_t> transmitted_;
    std::size_t poll_calls_{0U};
    std::size_t start_calls_{0U};
    std::size_t shutdown_calls_{0U};
};

} // namespace

int main() {
    lora::test::Runner runner;
    using lora::adapters::radio::CapLora1262Transport;
    using lora::ports::DatagramReceiveStatus;
    using lora::ports::DatagramSendStatus;

    runner.run("configuration and datagram bounds fail closed", [&] {
        FakeCapRadio invalid_radio;
        CapLora1262Transport invalid(
            invalid_radio, lora::ports::kMinimumDatagramMtu - 1U);
        CHECK(invalid.closed());
        CHECK_EQ(invalid.maximum_datagram_size(), 0U);
        CHECK_EQ(invalid_radio.shutdown_calls_, 1U);

        FakeCapRadio radio;
        CapLora1262Transport transport(
            radio, lora::ports::kMinimumDatagramMtu);
        REQUIRE(transport.connected());
        const std::uint8_t byte = 0x42U;
        CHECK_EQ(
            transport.try_send(nullptr, 1U),
            DatagramSendStatus::Invalid);
        CHECK_EQ(
            transport.try_send(&byte, 0U),
            DatagramSendStatus::Invalid);
        std::array<std::uint8_t, 49U> oversized{};
        CHECK_EQ(
            transport.try_send(oversized.data(), oversized.size()),
            DatagramSendStatus::Invalid);
        CHECK_EQ(radio.start_calls_, 0U);
    });

    runner.run("send maps accepted busy and hardware failure", [&] {
        FakeCapRadio radio;
        CapLora1262Transport transport(radio, 64U);
        const std::array<std::uint8_t, 4U> bytes{
            0x10U, 0x20U, 0x30U, 0x40U};

        REQUIRE(
            transport.try_send(bytes.data(), bytes.size()) ==
            DatagramSendStatus::Accepted);
        CHECK(std::equal(
            bytes.begin(), bytes.end(), radio.transmitted_.begin()));

        radio.start_status_ = RadioStartStatus::Busy;
        CHECK_EQ(
            transport.try_send(bytes.data(), bytes.size()),
            DatagramSendStatus::WouldBlock);
        CHECK(!transport.closed());

        radio.start_status_ = RadioStartStatus::Failed;
        CHECK_EQ(
            transport.try_send(bytes.data(), bytes.size()),
            DatagramSendStatus::Disconnected);
        CHECK(transport.closed());
        CHECK_EQ(radio.shutdown_calls_, 1U);
        CHECK_EQ(transport.metrics().accepted_transmits, 1U);
        CHECK_EQ(transport.metrics().busy_transmits, 1U);
        CHECK_EQ(transport.metrics().hardware_failures, 1U);
    });

    runner.run("receive preserves pending datagram on small destination", [&] {
        FakeCapRadio radio;
        CapLora1262Transport transport(radio, 64U);
        radio.push_poll(
            RadioPollStatus::Received,
            {0x01U, 0x02U, 0x03U, 0x04U});

        std::array<std::uint8_t, 8U> destination{};
        const auto too_small =
            transport.try_receive(destination.data(), 3U);
        CHECK_EQ(too_small.status, DatagramReceiveStatus::Invalid);
        CHECK_EQ(too_small.size, 0U);

        const auto received =
            transport.try_receive(
                destination.data(), destination.size());
        REQUIRE(
            received.status == DatagramReceiveStatus::Received);
        CHECK_EQ(received.size, 4U);
        CHECK_EQ(destination[0], 0x01U);
        CHECK_EQ(destination[3], 0x04U);
        CHECK_EQ(transport.metrics().received_datagrams, 1U);
    });

    runner.run("bounded receive slot drops overload and invalid frames", [&] {
        FakeCapRadio radio;
        CapLora1262Transport transport(radio, 48U);
        radio.push_poll(RadioPollStatus::Received, {0x11U});
        const std::array<std::uint8_t, 1U> outbound{0x99U};
        REQUIRE(
            transport.try_send(outbound.data(), outbound.size()) ==
            DatagramSendStatus::Accepted);

        radio.push_poll(RadioPollStatus::Received, {0x22U});
        REQUIRE(
            transport.try_send(outbound.data(), outbound.size()) ==
            DatagramSendStatus::Accepted);

        radio.push_poll(
            RadioPollStatus::Received, {}, 49U);
        REQUIRE(
            transport.try_send(outbound.data(), outbound.size()) ==
            DatagramSendStatus::Accepted);

        std::array<std::uint8_t, 4U> destination{};
        const auto received =
            transport.try_receive(
                destination.data(), destination.size());
        REQUIRE(
            received.status == DatagramReceiveStatus::Received);
        CHECK_EQ(destination[0], 0x11U);
        CHECK_EQ(transport.metrics().received_datagrams, 1U);
        CHECK_EQ(transport.metrics().dropped_datagrams, 2U);
    });

    runner.run("poll failure and close are idempotent", [&] {
        FakeCapRadio radio;
        CapLora1262Transport transport(radio, 48U);
        radio.push_poll(RadioPollStatus::Failed);
        std::array<std::uint8_t, 4U> destination{};
        CHECK_EQ(
            transport.try_receive(
                destination.data(), destination.size()).status,
            DatagramReceiveStatus::Disconnected);
        CHECK(transport.closed());
        CHECK_EQ(radio.shutdown_calls_, 1U);
        transport.close();
        transport.close();
        CHECK_EQ(radio.shutdown_calls_, 1U);
        CHECK_EQ(
            transport.try_send(destination.data(), 1U),
            DatagramSendStatus::Closed);
    });

    return runner.finish();
}
