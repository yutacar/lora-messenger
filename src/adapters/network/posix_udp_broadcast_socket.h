/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "adapters/network/udp_broadcast_transport.h"

#include <cstdint>
#include <memory>
#include <string>

namespace lora::adapters::network {

inline constexpr std::uint16_t kDefaultLanBroadcastPort = 42'425U;

struct PosixUdpBroadcastConfig {
    std::string interface_name{"wlan0"};
    std::uint16_t port{kDefaultLanBroadcastPort};
    bool receive_local_datagrams{false};

    bool valid() const noexcept;
};

// Nonblocking IPv4 UDP implementation for CardputerZero/Linux and POSIX test
// hosts. The directed broadcast address is rediscovered before every send, so
// DHCP address changes do not leave a stale destination. Received datagrams
// are accepted only from the selected interface's current subnet.
class PosixUdpBroadcastSocket final : public IUdpBroadcastSocket {
public:
    explicit PosixUdpBroadcastSocket(
        PosixUdpBroadcastConfig config = {}) noexcept;
    ~PosixUdpBroadcastSocket() override;

    PosixUdpBroadcastSocket(
        const PosixUdpBroadcastSocket&) = delete;
    PosixUdpBroadcastSocket& operator=(
        const PosixUdpBroadcastSocket&) = delete;

    bool ready() const noexcept override;
    bool connected() const noexcept override;
    UdpSocketSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept override;
    UdpSocketReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override;
    void close() noexcept override;

    const PosixUdpBroadcastConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lora::adapters::network
