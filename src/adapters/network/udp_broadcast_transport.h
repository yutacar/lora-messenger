/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/datagram_transport.h"

#include <cstddef>
#include <cstdint>

namespace lora::adapters::network {

enum class UdpSocketSendStatus {
    Sent,
    WouldBlock,
    Disconnected,
    Failed,
};

enum class UdpSocketReceiveStatus {
    Received,
    WouldBlock,
    Disconnected,
    Truncated,
    Failed,
};

struct UdpSocketReceiveResult {
    UdpSocketReceiveStatus status{UdpSocketReceiveStatus::WouldBlock};
    std::size_t size{0U};
};

// Narrow seam around a nonblocking, IPv4, local-LAN UDP broadcast socket.
// Implementations must reject datagrams from outside the configured local
// subnet before reporting Received.
class IUdpBroadcastSocket {
public:
    virtual ~IUdpBroadcastSocket() = default;

    virtual bool ready() const noexcept = 0;
    virtual bool connected() const noexcept = 0;
    virtual UdpSocketSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept = 0;
    virtual UdpSocketReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept = 0;
    virtual void close() noexcept = 0;
};

struct UdpBroadcastTransportMetrics {
    std::uint64_t accepted_transmits{0U};
    std::uint64_t backpressure_events{0U};
    std::uint64_t received_datagrams{0U};
    std::uint64_t dropped_datagrams{0U};
    std::uint64_t socket_failures{0U};
};

// Bounded, nonblocking IDatagramTransport adapter. The POSIX socket owns
// interface/subnet filtering; this layer owns the protocol MTU and maps socket
// outcomes without allocating or waiting.
class UdpBroadcastTransport final : public ports::IDatagramTransport {
public:
    explicit UdpBroadcastTransport(
        IUdpBroadcastSocket& socket,
        std::size_t mtu = ports::kMaximumDatagramBytes) noexcept;
    ~UdpBroadcastTransport() override;

    std::size_t maximum_datagram_size() const noexcept override;
    ports::DatagramSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept override;
    ports::DatagramReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override;
    bool connected() const noexcept override;
    bool closed() const noexcept override;
    void close() noexcept override;

    const UdpBroadcastTransportMetrics& metrics() const noexcept;

private:
    void fail_closed() noexcept;

    IUdpBroadcastSocket& socket_;
    std::size_t mtu_{0U};
    bool closed_{false};
    UdpBroadcastTransportMetrics metrics_;
};

} // namespace lora::adapters::network
