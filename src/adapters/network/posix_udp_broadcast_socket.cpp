/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/posix_udp_broadcast_socket.h"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lora::adapters::network {
namespace {

constexpr std::size_t kMaximumFilteredReceivesPerCall = 4U;

struct InterfaceIpv4 {
    in_addr address{};
    in_addr netmask{};
    in_addr broadcast{};
};

bool find_interface_ipv4(
    const std::string& name, InterfaceIpv4& result) noexcept {
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0 || !interfaces) {
        return false;
    }

    bool found = false;
    for (const ifaddrs* current = interfaces;
         current != nullptr; current = current->ifa_next) {
        if (!current->ifa_name || !current->ifa_addr ||
            !current->ifa_netmask ||
            current->ifa_addr->sa_family != AF_INET ||
            name != current->ifa_name ||
            (current->ifa_flags & IFF_UP) == 0 ||
            (current->ifa_flags & IFF_BROADCAST) == 0 ||
            !current->ifa_broadaddr) {
            continue;
        }
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(
                current->ifa_addr);
        const auto* netmask =
            reinterpret_cast<const sockaddr_in*>(
                current->ifa_netmask);
        const auto* broadcast =
            reinterpret_cast<const sockaddr_in*>(
                current->ifa_broadaddr);
        if (address->sin_addr.s_addr == htonl(INADDR_ANY) ||
            netmask->sin_addr.s_addr == htonl(INADDR_ANY) ||
            broadcast->sin_addr.s_addr == htonl(INADDR_ANY)) {
            continue;
        }
        result = {
            address->sin_addr,
            netmask->sin_addr,
            broadcast->sin_addr};
        found = true;
        break;
    }
    ::freeifaddrs(interfaces);
    return found;
}

bool same_subnet(
    const in_addr& left, const in_addr& right,
    const in_addr& netmask) noexcept {
    return (left.s_addr & netmask.s_addr) ==
           (right.s_addr & netmask.s_addr);
}

bool is_temporary_socket_error(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK ||
           error == EINTR;
}

bool is_disconnected_socket_error(int error) noexcept {
    return error == ENETDOWN || error == ENETUNREACH ||
           error == EHOSTDOWN || error == EHOSTUNREACH ||
           error == EADDRNOTAVAIL;
}

} // namespace

struct PosixUdpBroadcastSocket::Impl {
    explicit Impl(PosixUdpBroadcastConfig value) noexcept
        : config(std::move(value)) {}

    PosixUdpBroadcastConfig config;
    int descriptor{-1};
    mutable std::chrono::steady_clock::time_point
        last_interface_check{};
    mutable bool interface_check_started{false};
    mutable bool interface_connected{false};
};

bool PosixUdpBroadcastConfig::valid() const noexcept {
    return !interface_name.empty() &&
           interface_name.size() < IFNAMSIZ &&
           port >= 1024U;
}

PosixUdpBroadcastSocket::PosixUdpBroadcastSocket(
    PosixUdpBroadcastConfig config) noexcept
    : impl_(std::make_unique<Impl>(std::move(config))) {
    if (!impl_->config.valid()) {
        return;
    }

    const int descriptor =
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0) {
        return;
    }

    const int existing_flags =
        ::fcntl(descriptor, F_GETFL, 0);
    const int enabled = 1;
    if (existing_flags < 0 ||
        ::fcntl(
            descriptor, F_SETFL,
            existing_flags | O_NONBLOCK) < 0 ||
        ::setsockopt(
            descriptor, SOL_SOCKET, SO_REUSEADDR,
            &enabled, sizeof(enabled)) != 0 ||
        ::setsockopt(
            descriptor, SOL_SOCKET, SO_BROADCAST,
            &enabled, sizeof(enabled)) != 0) {
        ::close(descriptor);
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(impl_->config.port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(
            descriptor,
            reinterpret_cast<const sockaddr*>(&local),
            sizeof(local)) != 0) {
        ::close(descriptor);
        return;
    }
    impl_->descriptor = descriptor;
}

PosixUdpBroadcastSocket::~PosixUdpBroadcastSocket() {
    close();
}

bool PosixUdpBroadcastSocket::ready() const noexcept {
    return impl_ && impl_->descriptor >= 0;
}

bool PosixUdpBroadcastSocket::connected() const noexcept {
    if (!ready()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (impl_->interface_check_started &&
        now - impl_->last_interface_check <
            std::chrono::milliseconds(500)) {
        return impl_->interface_connected;
    }
    InterfaceIpv4 interface;
    impl_->interface_connected = find_interface_ipv4(
        impl_->config.interface_name, interface);
    impl_->last_interface_check = now;
    impl_->interface_check_started = true;
    return impl_->interface_connected;
}

UdpSocketSendStatus PosixUdpBroadcastSocket::try_send(
    const std::uint8_t* data, std::size_t size) noexcept {
    if (!ready() || !data || size == 0U ||
        size > ports::kMaximumDatagramBytes) {
        return UdpSocketSendStatus::Failed;
    }

    InterfaceIpv4 interface;
    if (!find_interface_ipv4(
            impl_->config.interface_name, interface)) {
        impl_->interface_connected = false;
        impl_->last_interface_check =
            std::chrono::steady_clock::now();
        impl_->interface_check_started = true;
        return UdpSocketSendStatus::Disconnected;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(impl_->config.port);
    destination.sin_addr = interface.broadcast;
    const auto sent = ::sendto(
        impl_->descriptor, data, size, 0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    if (sent >= 0) {
        return static_cast<std::size_t>(sent) == size
            ? UdpSocketSendStatus::Sent
            : UdpSocketSendStatus::Failed;
    }
    const int error = errno;
    if (is_temporary_socket_error(error)) {
        return UdpSocketSendStatus::WouldBlock;
    }
    if (is_disconnected_socket_error(error)) {
        impl_->interface_connected = false;
        impl_->last_interface_check =
            std::chrono::steady_clock::now();
        impl_->interface_check_started = true;
        return UdpSocketSendStatus::Disconnected;
    }
    return UdpSocketSendStatus::Failed;
}

UdpSocketReceiveResult
PosixUdpBroadcastSocket::try_receive(
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    if (!ready() || !destination ||
        destination_capacity == 0U) {
        return {UdpSocketReceiveStatus::Failed, 0U};
    }

    for (std::size_t attempt = 0U;
         attempt < kMaximumFilteredReceivesPerCall; ++attempt) {
        sockaddr_in source{};
        socklen_t source_size = sizeof(source);
        const auto received = ::recvfrom(
            impl_->descriptor, destination,
            destination_capacity, MSG_TRUNC,
            reinterpret_cast<sockaddr*>(&source),
            &source_size);
        if (received < 0) {
            const int error = errno;
            if (is_temporary_socket_error(error)) {
                return {
                    UdpSocketReceiveStatus::WouldBlock, 0U};
            }
            if (is_disconnected_socket_error(error)) {
                impl_->interface_connected = false;
                impl_->last_interface_check =
                    std::chrono::steady_clock::now();
                impl_->interface_check_started = true;
                return {
                    UdpSocketReceiveStatus::Disconnected, 0U};
            }
            return {
                UdpSocketReceiveStatus::Failed,
                0U};
        }
        const auto received_size =
            static_cast<std::uint64_t>(received);
        if (received_size >
                static_cast<std::uint64_t>(
                    destination_capacity) ||
            received_size >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            return {
                UdpSocketReceiveStatus::Truncated,
                destination_capacity};
        }
        if (source_size < sizeof(sockaddr_in) ||
            source.sin_family != AF_INET ||
            ntohs(source.sin_port) != impl_->config.port) {
            continue;
        }

        InterfaceIpv4 interface;
        if (!find_interface_ipv4(
                impl_->config.interface_name, interface)) {
            impl_->interface_connected = false;
            impl_->last_interface_check =
                std::chrono::steady_clock::now();
            impl_->interface_check_started = true;
            return {
                UdpSocketReceiveStatus::Disconnected, 0U};
        }
        if (!same_subnet(
                source.sin_addr, interface.address,
                interface.netmask) ||
            (!impl_->config.receive_local_datagrams &&
             source.sin_addr.s_addr ==
                 interface.address.s_addr)) {
            continue;
        }
        return {
            UdpSocketReceiveStatus::Received,
            static_cast<std::size_t>(received_size)};
    }
    return {UdpSocketReceiveStatus::WouldBlock, 0U};
}

void PosixUdpBroadcastSocket::close() noexcept {
    if (!impl_ || impl_->descriptor < 0) {
        return;
    }
    ::close(impl_->descriptor);
    impl_->descriptor = -1;
}

const PosixUdpBroadcastConfig&
PosixUdpBroadcastSocket::config() const noexcept {
    return impl_->config;
}

} // namespace lora::adapters::network
