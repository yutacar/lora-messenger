/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/udp_broadcast_transport.h"

#include "test_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace {

using lora::adapters::network::IUdpBroadcastSocket;
using lora::adapters::network::UdpSocketReceiveResult;
using lora::adapters::network::UdpSocketReceiveStatus;
using lora::adapters::network::UdpSocketSendStatus;

class FakeUdpSocket final : public IUdpBroadcastSocket {
public:
    bool ready() const noexcept override {
        return ready_;
    }

    bool connected() const noexcept override {
        return ready_ && connected_;
    }

    UdpSocketSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept override {
        ++send_calls_;
        if (send_status_ == UdpSocketSendStatus::Sent) {
            sent_.assign(data, data + size);
        }
        return send_status_;
    }

    UdpSocketReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override {
        ++receive_calls_;
        if (receive_.empty()) {
            return {};
        }
        const auto scripted = receive_.front();
        receive_.pop_front();
        if (scripted.status == UdpSocketReceiveStatus::Received &&
            scripted.bytes.size() <= destination_capacity) {
            std::copy(
                scripted.bytes.begin(), scripted.bytes.end(),
                destination);
            return {scripted.status, scripted.bytes.size()};
        }
        return {scripted.status, scripted.reported_size};
    }

    void close() noexcept override {
        ++close_calls_;
        ready_ = false;
    }

    struct ScriptedReceive {
        UdpSocketReceiveStatus status{
            UdpSocketReceiveStatus::WouldBlock};
        std::vector<std::uint8_t> bytes;
        std::size_t reported_size{0U};
    };

    bool ready_{true};
    bool connected_{true};
    UdpSocketSendStatus send_status_{UdpSocketSendStatus::Sent};
    std::deque<ScriptedReceive> receive_;
    std::vector<std::uint8_t> sent_;
    std::size_t send_calls_{0U};
    std::size_t receive_calls_{0U};
    std::size_t close_calls_{0U};
};

} // namespace

int main() {
    lora::test::Runner runner;
    using lora::adapters::network::UdpBroadcastTransport;
    using lora::ports::DatagramReceiveStatus;
    using lora::ports::DatagramSendStatus;

    runner.run("invalid construction fails closed", [&] {
        FakeUdpSocket socket;
        UdpBroadcastTransport transport(
            socket, lora::ports::kMinimumDatagramMtu - 1U);
        CHECK(transport.closed());
        CHECK_EQ(transport.maximum_datagram_size(), 0U);
        CHECK_EQ(socket.close_calls_, 1U);
    });

    runner.run("send validates bounds and maps socket outcomes", [&] {
        FakeUdpSocket socket;
        UdpBroadcastTransport transport(socket, 64U);
        const std::array<std::uint8_t, 4U> bytes{
            0x10U, 0x20U, 0x30U, 0x40U};
        CHECK_EQ(
            transport.try_send(nullptr, 1U),
            DatagramSendStatus::Invalid);
        CHECK_EQ(
            transport.try_send(bytes.data(), 0U),
            DatagramSendStatus::Invalid);

        REQUIRE(
            transport.try_send(bytes.data(), bytes.size()) ==
            DatagramSendStatus::Accepted);
        CHECK_EQ(socket.sent_, std::vector<std::uint8_t>(
                                   bytes.begin(), bytes.end()));

        socket.send_status_ = UdpSocketSendStatus::WouldBlock;
        CHECK_EQ(
            transport.try_send(bytes.data(), bytes.size()),
            DatagramSendStatus::WouldBlock);
        socket.send_status_ = UdpSocketSendStatus::Disconnected;
        CHECK_EQ(
            transport.try_send(bytes.data(), bytes.size()),
            DatagramSendStatus::Disconnected);
        CHECK(!transport.closed());
        CHECK_EQ(transport.metrics().accepted_transmits, 1U);
        CHECK_EQ(transport.metrics().backpressure_events, 1U);
    });

    runner.run("temporary interface loss does not close the transport", [&] {
        FakeUdpSocket socket;
        socket.connected_ = false;
        UdpBroadcastTransport transport(socket, 64U);
        CHECK(!transport.connected());
        CHECK(!transport.closed());
        CHECK_EQ(transport.maximum_datagram_size(), 64U);
        socket.connected_ = true;
        CHECK(transport.connected());
    });

    runner.run("receive maps valid datagrams and drops truncation", [&] {
        FakeUdpSocket socket;
        UdpBroadcastTransport transport(socket, 48U);
        socket.receive_.push_back(
            {UdpSocketReceiveStatus::Received,
             {0x01U, 0x02U, 0x03U}, 0U});
        socket.receive_.push_back(
            {UdpSocketReceiveStatus::Truncated, {}, 80U});

        std::array<std::uint8_t, 64U> destination{};
        const auto received =
            transport.try_receive(
                destination.data(), destination.size());
        REQUIRE(
            received.status == DatagramReceiveStatus::Received);
        CHECK_EQ(received.size, 3U);
        CHECK_EQ(destination[2], 0x03U);

        CHECK_EQ(
            transport.try_receive(
                destination.data(), destination.size()).status,
            DatagramReceiveStatus::WouldBlock);
        CHECK_EQ(transport.metrics().received_datagrams, 1U);
        CHECK_EQ(transport.metrics().dropped_datagrams, 1U);
    });

    runner.run("fatal socket failure and close are idempotent", [&] {
        FakeUdpSocket socket;
        UdpBroadcastTransport transport(socket, 48U);
        socket.send_status_ = UdpSocketSendStatus::Failed;
        const std::uint8_t byte = 0x42U;
        CHECK_EQ(
            transport.try_send(&byte, 1U),
            DatagramSendStatus::Disconnected);
        CHECK(transport.closed());
        CHECK_EQ(transport.metrics().socket_failures, 1U);
        CHECK_EQ(socket.close_calls_, 1U);
        transport.close();
        transport.close();
        CHECK_EQ(socket.close_calls_, 1U);
    });

    return runner.finish();
}
