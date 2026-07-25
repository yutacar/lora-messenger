/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/datagram_transport.h"

#include <cstddef>
#include <cstdint>

namespace lora::adapters::radio {

enum class RadioStartStatus {
    Started,
    Busy,
    Failed,
};

enum class RadioPollStatus {
    Idle,
    TransmitComplete,
    Received,
    Failed,
};

struct RadioPollResult {
    RadioPollStatus status{RadioPollStatus::Idle};
    std::size_t size{0U};
};

// Narrow hardware seam for the Cap LoRa-1262 SX1262. Implementations own SPI,
// GPIO, the Cap's PI4IOE5V6408 antenna switch, and receive-mode restoration.
class ICapLora1262Radio {
public:
    virtual ~ICapLora1262Radio() = default;

    virtual bool ready() const noexcept = 0;
    virtual RadioStartStatus try_start_transmit(
        const std::uint8_t* data, std::size_t size) noexcept = 0;
    virtual RadioPollResult poll(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

struct CapLora1262TransportMetrics {
    std::uint64_t accepted_transmits{0U};
    std::uint64_t busy_transmits{0U};
    std::uint64_t received_datagrams{0U};
    std::uint64_t dropped_datagrams{0U};
    std::uint64_t hardware_failures{0U};
};

// Bounded nonblocking adapter. It intentionally buffers at most one received
// datagram: the application must drain promptly, and overload drops are visible
// through metrics instead of growing an unbounded queue.
class CapLora1262Transport final : public ports::IDatagramTransport {
public:
    explicit CapLora1262Transport(
        ICapLora1262Radio& radio,
        std::size_t mtu = ports::kMaximumDatagramBytes) noexcept;
    ~CapLora1262Transport() override;

    std::size_t maximum_datagram_size() const noexcept override;
    ports::DatagramSendStatus try_send(
        const std::uint8_t* data, std::size_t size) noexcept override;
    ports::DatagramReceiveResult try_receive(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override;
    bool connected() const noexcept override;
    bool closed() const noexcept override;
    void close() noexcept override;

    const CapLora1262TransportMetrics& metrics() const noexcept;

private:
    void service_radio() noexcept;
    void fail_closed() noexcept;

    ICapLora1262Radio& radio_;
    std::size_t mtu_{0U};
    std::uint8_t pending_receive_[ports::kMaximumDatagramBytes]{};
    std::size_t pending_receive_size_{0U};
    bool closed_{false};
    CapLora1262TransportMetrics metrics_;
};

} // namespace lora::adapters::radio
