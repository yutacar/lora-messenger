/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/udp_broadcast_transport.h"

namespace lora::adapters::network {

UdpBroadcastTransport::UdpBroadcastTransport(
    IUdpBroadcastSocket& socket, std::size_t mtu) noexcept
    : socket_(socket) {
    if (mtu >= ports::kMinimumDatagramMtu &&
        mtu <= ports::kMaximumDatagramBytes &&
        socket_.ready()) {
        mtu_ = mtu;
    } else {
        closed_ = true;
        socket_.close();
    }
}

UdpBroadcastTransport::~UdpBroadcastTransport() {
    close();
}

std::size_t UdpBroadcastTransport::maximum_datagram_size() const noexcept {
    return closed_ ? 0U : mtu_;
}

ports::DatagramSendStatus UdpBroadcastTransport::try_send(
    const std::uint8_t* data, std::size_t size) noexcept {
    if (closed_) {
        return ports::DatagramSendStatus::Closed;
    }
    if (!data || size == 0U || size > mtu_) {
        return ports::DatagramSendStatus::Invalid;
    }

    switch (socket_.try_send(data, size)) {
        case UdpSocketSendStatus::Sent:
            ++metrics_.accepted_transmits;
            return ports::DatagramSendStatus::Accepted;
        case UdpSocketSendStatus::WouldBlock:
            ++metrics_.backpressure_events;
            return ports::DatagramSendStatus::WouldBlock;
        case UdpSocketSendStatus::Disconnected:
            return ports::DatagramSendStatus::Disconnected;
        case UdpSocketSendStatus::Failed:
            fail_closed();
            return ports::DatagramSendStatus::Disconnected;
    }
    fail_closed();
    return ports::DatagramSendStatus::Disconnected;
}

ports::DatagramReceiveResult UdpBroadcastTransport::try_receive(
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    if (closed_) {
        return {ports::DatagramReceiveStatus::Closed, 0U};
    }
    if (!destination || destination_capacity == 0U) {
        return {ports::DatagramReceiveStatus::Invalid, 0U};
    }

    const auto result =
        socket_.try_receive(destination, destination_capacity);
    switch (result.status) {
        case UdpSocketReceiveStatus::Received:
            if (result.size == 0U || result.size > mtu_ ||
                result.size > destination_capacity) {
                ++metrics_.dropped_datagrams;
                return {
                    ports::DatagramReceiveStatus::WouldBlock, 0U};
            }
            ++metrics_.received_datagrams;
            return {
                ports::DatagramReceiveStatus::Received, result.size};
        case UdpSocketReceiveStatus::WouldBlock:
            return {
                ports::DatagramReceiveStatus::WouldBlock, 0U};
        case UdpSocketReceiveStatus::Disconnected:
            return {
                ports::DatagramReceiveStatus::Disconnected, 0U};
        case UdpSocketReceiveStatus::Truncated:
            ++metrics_.dropped_datagrams;
            return {
                ports::DatagramReceiveStatus::WouldBlock, 0U};
        case UdpSocketReceiveStatus::Failed:
            fail_closed();
            return {
                ports::DatagramReceiveStatus::Disconnected, 0U};
    }
    fail_closed();
    return {ports::DatagramReceiveStatus::Disconnected, 0U};
}

bool UdpBroadcastTransport::connected() const noexcept {
    return !closed_ && socket_.ready() && socket_.connected();
}

bool UdpBroadcastTransport::closed() const noexcept {
    return closed_;
}

void UdpBroadcastTransport::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    socket_.close();
}

const UdpBroadcastTransportMetrics&
UdpBroadcastTransport::metrics() const noexcept {
    return metrics_;
}

void UdpBroadcastTransport::fail_closed() noexcept {
    if (!closed_) {
        ++metrics_.socket_failures;
        closed_ = true;
        socket_.close();
    }
}

} // namespace lora::adapters::network
