/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lora::ports {

// Phase 4 exercises the protocol at the smallest planned simulated-radio MTU
// and never permits an adapter to expose a datagram larger than one byte-sized
// wire length can represent.
inline constexpr std::size_t kMinimumDatagramMtu = 48U;
inline constexpr std::size_t kMaximumDatagramBytes = 255U;

enum class DatagramSendStatus {
    Accepted,
    WouldBlock,
    Disconnected,
    Closed,
    Invalid,
};

enum class DatagramReceiveStatus {
    Received,
    WouldBlock,
    Disconnected,
    Closed,
    Invalid,
};

struct DatagramReceiveResult {
    DatagramReceiveStatus status{DatagramReceiveStatus::WouldBlock};
    std::size_t size{0U};
};

// A bounded, nonblocking datagram boundary. The exposed value is the fixed
// logical-link MTU used by both peers for canonical fragmentation, not a
// negotiable per-message ceiling. Accepted means the adapter accepted the
// datagram; it is not an acknowledgement that a peer received it.
class IDatagramTransport {
public:
    virtual ~IDatagramTransport() = default;

    virtual std::size_t maximum_datagram_size() const noexcept = 0;

    virtual DatagramSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept = 0;

    // A too-small destination is Invalid and must leave the pending datagram
    // available for a later call with sufficient capacity.
    virtual DatagramReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept = 0;

    virtual bool connected() const noexcept = 0;
    virtual bool closed() const noexcept = 0;
    virtual void close() noexcept = 0;
};

} // namespace lora::ports
