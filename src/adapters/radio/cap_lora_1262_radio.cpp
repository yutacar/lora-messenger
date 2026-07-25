/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/radio/cap_lora_1262_radio.h"

#include <algorithm>

namespace lora::adapters::radio {

CapLora1262Transport::CapLora1262Transport(
    ICapLora1262Radio& radio, std::size_t mtu) noexcept
    : radio_(radio) {
    if (mtu >= ports::kMinimumDatagramMtu &&
        mtu <= ports::kMaximumDatagramBytes &&
        radio_.ready()) {
        mtu_ = mtu;
    } else {
        closed_ = true;
        radio_.shutdown();
    }
}

CapLora1262Transport::~CapLora1262Transport() {
    close();
}

std::size_t CapLora1262Transport::maximum_datagram_size() const noexcept {
    return closed_ ? 0U : mtu_;
}

ports::DatagramSendStatus CapLora1262Transport::try_send(
    const std::uint8_t* data, std::size_t size) noexcept {
    if (closed_) {
        return ports::DatagramSendStatus::Closed;
    }
    if (!data || size == 0U || size > mtu_) {
        return ports::DatagramSendStatus::Invalid;
    }

    service_radio();
    if (closed_) {
        return ports::DatagramSendStatus::Disconnected;
    }

    switch (radio_.try_start_transmit(data, size)) {
        case RadioStartStatus::Started:
            ++metrics_.accepted_transmits;
            return ports::DatagramSendStatus::Accepted;
        case RadioStartStatus::Busy:
            ++metrics_.busy_transmits;
            return ports::DatagramSendStatus::WouldBlock;
        case RadioStartStatus::Failed:
            fail_closed();
            return ports::DatagramSendStatus::Disconnected;
    }
    fail_closed();
    return ports::DatagramSendStatus::Disconnected;
}

ports::DatagramReceiveResult CapLora1262Transport::try_receive(
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    if (closed_) {
        return {ports::DatagramReceiveStatus::Closed, 0U};
    }
    if (!destination || destination_capacity == 0U) {
        return {ports::DatagramReceiveStatus::Invalid, 0U};
    }

    service_radio();
    if (closed_) {
        return {ports::DatagramReceiveStatus::Disconnected, 0U};
    }
    if (pending_receive_size_ == 0U) {
        return {ports::DatagramReceiveStatus::WouldBlock, 0U};
    }
    if (destination_capacity < pending_receive_size_) {
        return {ports::DatagramReceiveStatus::Invalid, 0U};
    }

    const auto size = pending_receive_size_;
    std::copy_n(pending_receive_, size, destination);
    pending_receive_size_ = 0U;
    return {ports::DatagramReceiveStatus::Received, size};
}

bool CapLora1262Transport::connected() const noexcept {
    return !closed_ && radio_.ready();
}

bool CapLora1262Transport::closed() const noexcept {
    return closed_;
}

void CapLora1262Transport::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    pending_receive_size_ = 0U;
    radio_.shutdown();
}

const CapLora1262TransportMetrics&
CapLora1262Transport::metrics() const noexcept {
    return metrics_;
}

void CapLora1262Transport::service_radio() noexcept {
    if (closed_) {
        return;
    }

    std::uint8_t bytes[ports::kMaximumDatagramBytes]{};
    const auto result = radio_.poll(bytes, sizeof(bytes));
    switch (result.status) {
        case RadioPollStatus::Idle:
        case RadioPollStatus::TransmitComplete:
            return;
        case RadioPollStatus::Received:
            if (result.size == 0U || result.size > mtu_) {
                ++metrics_.dropped_datagrams;
                return;
            }
            if (pending_receive_size_ != 0U) {
                ++metrics_.dropped_datagrams;
                return;
            }
            std::copy_n(bytes, result.size, pending_receive_);
            pending_receive_size_ = result.size;
            ++metrics_.received_datagrams;
            return;
        case RadioPollStatus::Failed:
            fail_closed();
            return;
    }
    fail_closed();
}

void CapLora1262Transport::fail_closed() noexcept {
    if (!closed_) {
        ++metrics_.hardware_failures;
        closed_ = true;
        pending_receive_size_ = 0U;
        radio_.shutdown();
    }
}

} // namespace lora::adapters::radio
