/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/transport/simulated_radio_bus.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using lora::adapters::transport::SimulatedRadioBus;
using lora::ports::DatagramReceiveStatus;
using lora::ports::DatagramSendStatus;

std::array<std::uint8_t, lora::ports::kMaximumDatagramBytes>
payload(std::size_t size, std::uint8_t start) {
    std::array<std::uint8_t, lora::ports::kMaximumDatagramBytes> result{};
    for (std::size_t index = 0U; index < size; ++index) {
        result[index] =
            static_cast<std::uint8_t>(start +
                                      static_cast<std::uint8_t>(index));
    }
    return result;
}

} // namespace

int main() {
    lora::test::Runner runner;

    runner.run("configuration and datagram inputs fail closed", [&] {
        SimulatedRadioBus too_small({
            lora::ports::kMinimumDatagramMtu - 1U, 1U, 1U, 1U});
        CHECK(!too_small.valid());
        CHECK_EQ(too_small.first_endpoint().maximum_datagram_size(), 0U);
        const std::uint8_t byte = 0x55U;
        CHECK_EQ(too_small.first_endpoint().try_send(&byte, 1U),
                 DatagramSendStatus::Invalid);
        CHECK(!too_small.advance_to(0U));

        SimulatedRadioBus too_large({
            lora::ports::kMaximumDatagramBytes + 1U, 1U, 1U, 1U});
        CHECK(!too_large.valid());

        SimulatedRadioBus zero_depth({
            lora::ports::kMinimumDatagramMtu, 0U, 1U, 1U});
        CHECK(!zero_depth.valid());

        SimulatedRadioBus excessive_depth({
            lora::ports::kMinimumDatagramMtu,
            SimulatedRadioBus::kMaximumQueueDepth + 1U, 1U, 1U});
        CHECK(!excessive_depth.valid());

        SimulatedRadioBus bus({
            lora::ports::kMinimumDatagramMtu, 4U, 4U, 4U});
        REQUIRE(bus.valid());
        auto& first = bus.first_endpoint();
        CHECK_EQ(first.maximum_datagram_size(),
                 lora::ports::kMinimumDatagramMtu);
        CHECK_EQ(first.try_send(nullptr, 1U),
                 DatagramSendStatus::Invalid);
        CHECK_EQ(first.try_send(&byte, 0U),
                 DatagramSendStatus::Invalid);
        const auto oversized = payload(
            lora::ports::kMinimumDatagramMtu + 1U, 0x10U);
        CHECK_EQ(first.try_send(oversized.data(),
                                lora::ports::kMinimumDatagramMtu + 1U),
                 DatagramSendStatus::Invalid);
        CHECK_EQ(bus.statistics().invalid_sends, 3U);
    });

    runner.run("immediate datagrams preserve bytes and FIFO direction", [&] {
        constexpr std::size_t mtu = 48U;
        SimulatedRadioBus bus({mtu, 4U, 4U, 4U});
        auto& first = bus.first_endpoint();
        auto& second = bus.second_endpoint();
        const auto outbound = payload(mtu, 0x20U);

        REQUIRE(first.try_send(outbound.data(), mtu) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.scheduled_event_count(), 0U);
        CHECK_EQ(bus.inbound_datagram_count(
                     SimulatedRadioBus::EndpointId::Second),
                 1U);

        std::array<std::uint8_t,
                   lora::ports::kMaximumDatagramBytes> received{};
        const auto too_small =
            second.try_receive(received.data(), mtu - 1U);
        CHECK_EQ(too_small.status, DatagramReceiveStatus::Invalid);
        CHECK_EQ(too_small.size, 0U);
        CHECK_EQ(bus.inbound_datagram_count(
                     SimulatedRadioBus::EndpointId::Second),
                 1U);

        const auto result =
            second.try_receive(received.data(), received.size());
        REQUIRE(result.status == DatagramReceiveStatus::Received);
        CHECK_EQ(result.size, mtu);
        CHECK(std::equal(outbound.begin(), outbound.begin() +
                            static_cast<std::ptrdiff_t>(mtu),
                         received.begin()));
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::WouldBlock);

        const auto reverse = payload(3U, 0xa0U);
        REQUIRE(second.try_send(reverse.data(), 3U) ==
                DatagramSendStatus::Accepted);
        const auto reverse_result =
            first.try_receive(received.data(), received.size());
        REQUIRE(reverse_result.status == DatagramReceiveStatus::Received);
        CHECK_EQ(reverse_result.size, 3U);
        CHECK(std::equal(reverse.begin(), reverse.begin() + 3,
                         received.begin()));
    });

    runner.run("all phase 4 MTUs enforce their immutable byte boundary", [&] {
        constexpr std::array<std::size_t, 5U> mtus{
            48U, 51U, 64U, 128U, 255U};
        for (const auto mtu : mtus) {
            SimulatedRadioBus bus({mtu, 2U, 2U, 2U});
            REQUIRE(bus.valid());
            auto& first = bus.first_endpoint();
            auto& second = bus.second_endpoint();
            CHECK_EQ(first.maximum_datagram_size(), mtu);

            const auto bytes = payload(mtu, 0U);
            REQUIRE(first.try_send(bytes.data(), mtu) ==
                    DatagramSendStatus::Accepted);
            std::array<std::uint8_t,
                       lora::ports::kMaximumDatagramBytes> received{};
            const auto result =
                second.try_receive(received.data(), received.size());
            REQUIRE(result.status == DatagramReceiveStatus::Received);
            CHECK_EQ(result.size, mtu);
            CHECK(std::equal(bytes.begin(),
                             bytes.begin() +
                                 static_cast<std::ptrdiff_t>(mtu),
                             received.begin()));

            if (mtu < lora::ports::kMaximumDatagramBytes) {
                CHECK_EQ(first.try_send(bytes.data(), mtu + 1U),
                         DatagramSendStatus::Invalid);
            }
        }
    });

    runner.run("scheduled capacity is atomic and preserves the fault", [&] {
        SimulatedRadioBus bus({64U, 2U, 4U, 4U});
        auto& first = bus.first_endpoint();
        auto& second = bus.second_endpoint();
        const auto one = payload(4U, 0x10U);
        const auto two = payload(4U, 0x30U);

        SimulatedRadioBus::FaultDirective delayed;
        delayed.delay_ticks = 10U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, delayed));
        REQUIRE(first.try_send(one.data(), 4U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.scheduled_event_count(), 1U);

        SimulatedRadioBus::FaultDirective duplicate;
        duplicate.duplicate = true;
        duplicate.delay_ticks = 10U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, duplicate));
        CHECK_EQ(first.try_send(two.data(), 4U),
                 DatagramSendStatus::WouldBlock);
        CHECK_EQ(bus.fault_directive_count(
                     SimulatedRadioBus::EndpointId::First),
                 1U);
        CHECK_EQ(bus.scheduled_event_count(), 1U);

        REQUIRE(bus.advance_to(10U));
        std::array<std::uint8_t,
                   lora::ports::kMaximumDatagramBytes> received{};
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);
        CHECK(std::equal(one.begin(), one.begin() + 4, received.begin()));

        REQUIRE(first.try_send(two.data(), 4U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.fault_directive_count(
                     SimulatedRadioBus::EndpointId::First),
                 0U);
        CHECK_EQ(bus.scheduled_event_count(), 2U);
        REQUIRE(bus.advance_to(20U));
        for (int copy = 0; copy < 2; ++copy) {
            const auto result =
                second.try_receive(received.data(), received.size());
            REQUIRE(result.status == DatagramReceiveStatus::Received);
            CHECK(std::equal(two.begin(), two.begin() + 4,
                             received.begin()));
        }
        CHECK_EQ(bus.statistics().would_block_sends, 1U);
        CHECK_EQ(bus.statistics().scheduled_high_watermark, 2U);
    });

    runner.run("fault directives drop duplicate corrupt delay and reorder", [&] {
        SimulatedRadioBus bus({64U, 8U, 8U, 8U});
        auto& first = bus.first_endpoint();
        auto& second = bus.second_endpoint();
        std::array<std::uint8_t,
                   lora::ports::kMaximumDatagramBytes> received{};

        const auto dropped = payload(3U, 0x10U);
        SimulatedRadioBus::FaultDirective drop;
        drop.drop = true;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, drop));
        REQUIRE(first.try_send(dropped.data(), 3U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::WouldBlock);

        const auto duplicated = payload(3U, 0x20U);
        SimulatedRadioBus::FaultDirective duplicate;
        duplicate.duplicate = true;
        duplicate.delay_ticks = 5U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, duplicate));
        REQUIRE(first.try_send(duplicated.data(), 3U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(bus.advance_to(4U));
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::WouldBlock);
        REQUIRE(bus.advance_to(5U));
        for (int copy = 0; copy < 2; ++copy) {
            const auto result =
                second.try_receive(received.data(), received.size());
            REQUIRE(result.status == DatagramReceiveStatus::Received);
            CHECK(std::equal(duplicated.begin(), duplicated.begin() + 3,
                             received.begin()));
        }

        const auto original = payload(4U, 0x40U);
        SimulatedRadioBus::FaultDirective corrupt;
        corrupt.corrupt_byte_index = 1U;
        corrupt.corrupt_xor_mask = 0x80U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, corrupt));
        REQUIRE(first.try_send(original.data(), 4U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);
        CHECK_EQ(received[0], original[0]);
        CHECK_EQ(received[1],
                 static_cast<std::uint8_t>(original[1] ^ 0x80U));
        CHECK_EQ(received[2], original[2]);
        CHECK_EQ(received[3], original[3]);

        const auto slow = payload(2U, 0x60U);
        const auto fast = payload(2U, 0x70U);
        SimulatedRadioBus::FaultDirective delay;
        delay.delay_ticks = 10U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, delay));
        REQUIRE(first.try_send(slow.data(), 2U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(first.try_send(fast.data(), 2U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);
        CHECK(std::equal(fast.begin(), fast.begin() + 2,
                         received.begin()));
        REQUIRE(bus.advance_to(15U));
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);
        CHECK(std::equal(slow.begin(), slow.begin() + 2,
                         received.begin()));

        CHECK_EQ(bus.statistics().fault_drops, 1U);
        CHECK_EQ(bus.statistics().duplicate_copies, 1U);
        CHECK_EQ(bus.statistics().corrupted_copies, 1U);
        CHECK_EQ(bus.statistics().delivered_datagrams, 5U);
    });

    runner.run("fault and inbound queues stay bounded", [&] {
        SimulatedRadioBus bus({48U, 4U, 1U, 1U});
        auto& first = bus.first_endpoint();
        auto& second = bus.second_endpoint();
        const auto one = payload(1U, 0x11U);
        const auto two = payload(1U, 0x22U);

        SimulatedRadioBus::FaultDirective drop;
        drop.drop = true;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, drop));
        CHECK(!bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, drop));
        REQUIRE(first.try_send(one.data(), 1U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.fault_directive_count(
                     SimulatedRadioBus::EndpointId::First),
                 0U);

        REQUIRE(first.try_send(one.data(), 1U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(first.try_send(two.data(), 1U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.inbound_datagram_count(
                     SimulatedRadioBus::EndpointId::Second),
                 1U);
        CHECK_EQ(bus.statistics().inbound_full_drops, 1U);
        CHECK_EQ(bus.statistics().inbound_high_watermarks[1], 1U);

        std::array<std::uint8_t,
                   lora::ports::kMaximumDatagramBytes> received{};
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);
        CHECK_EQ(received[0], one[0]);

        SimulatedRadioBus::FaultDirective bad_index;
        bad_index.corrupt_byte_index = 48U;
        CHECK(!bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, bad_index));
        SimulatedRadioBus::FaultDirective bad_mask;
        bad_mask.corrupt_byte_index = 0U;
        bad_mask.corrupt_xor_mask = 0U;
        CHECK(!bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, bad_mask));
    });

    runner.run("disconnect reconnect and close discard stale link data", [&] {
        SimulatedRadioBus bus({64U, 8U, 8U, 8U});
        auto& first = bus.first_endpoint();
        auto& second = bus.second_endpoint();
        const auto bytes = payload(3U, 0x80U);

        SimulatedRadioBus::FaultDirective delay;
        delay.delay_ticks = 20U;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, delay));
        REQUIRE(first.try_send(bytes.data(), 3U) ==
                DatagramSendStatus::Accepted);
        CHECK_EQ(bus.scheduled_event_count(), 1U);
        REQUIRE(bus.set_connected(
            SimulatedRadioBus::EndpointId::Second, false));
        CHECK_EQ(bus.scheduled_event_count(), 0U);
        CHECK(!first.connected());
        CHECK(!second.connected());
        CHECK_EQ(first.try_send(bytes.data(), 3U),
                 DatagramSendStatus::Disconnected);

        std::array<std::uint8_t,
                   lora::ports::kMaximumDatagramBytes> received{};
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::Disconnected);
        REQUIRE(bus.set_connected(
            SimulatedRadioBus::EndpointId::Second, true));
        CHECK(first.connected());
        CHECK(second.connected());
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::WouldBlock);

        REQUIRE(first.try_send(bytes.data(), 3U) ==
                DatagramSendStatus::Accepted);
        REQUIRE(second.try_receive(received.data(), received.size()).status ==
                DatagramReceiveStatus::Received);

        first.close();
        CHECK(first.closed());
        CHECK(!first.connected());
        CHECK(!second.connected());
        CHECK_EQ(first.try_send(bytes.data(), 3U),
                 DatagramSendStatus::Closed);
        CHECK_EQ(second.try_send(bytes.data(), 3U),
                 DatagramSendStatus::Disconnected);
        CHECK(!bus.set_connected(
            SimulatedRadioBus::EndpointId::First, true));
        second.close();
        CHECK_EQ(second.try_receive(received.data(), received.size()).status,
                 DatagramReceiveStatus::Closed);
        CHECK(bus.statistics().disconnected_drops >= 1U);
    });

    runner.run("virtual time is monotonic and delay overflow is rejected", [&] {
        SimulatedRadioBus bus({48U, 2U, 2U, 2U});
        REQUIRE(bus.advance_to(7U));
        CHECK_EQ(bus.current_tick(), 7U);
        CHECK(!bus.advance_to(6U));
        CHECK_EQ(bus.current_tick(), 7U);
        REQUIRE(bus.advance_to(std::numeric_limits<
                               SimulatedRadioBus::Tick>::max()));

        SimulatedRadioBus::FaultDirective overflow;
        overflow.delay_ticks = 1U;
        CHECK(!bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, overflow));

        SimulatedRadioBus::FaultDirective immediate;
        REQUIRE(bus.enqueue_fault(
            SimulatedRadioBus::EndpointId::First, immediate));
        const std::uint8_t value = 0x5aU;
        REQUIRE(bus.first_endpoint().try_send(&value, 1U) ==
                DatagramSendStatus::Accepted);
        std::uint8_t received = 0U;
        const auto result =
            bus.second_endpoint().try_receive(&received, 1U);
        REQUIRE(result.status == DatagramReceiveStatus::Received);
        CHECK_EQ(result.size, 1U);
        CHECK_EQ(received, value);
    });

    return runner.finish();
}
