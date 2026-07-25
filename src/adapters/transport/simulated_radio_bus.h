/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/datagram_transport.h"
#include "ports/radio_policy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lora::adapters::transport {

// A deterministic two-endpoint datagram bus for protocol tests. It owns no
// thread or clock: callers explicitly advance its virtual tick.
class SimulatedRadioBus {
public:
    using Tick = ports::RadioTick;

    inline static constexpr std::size_t kMaximumQueueDepth = 4096U;

    enum class EndpointId : std::uint8_t {
        First = 0U,
        Second = 1U,
    };

    struct Config {
        std::size_t mtu{ports::kMaximumDatagramBytes};
        std::size_t max_scheduled_events{64U};
        std::size_t max_inbound_datagrams{64U};
        std::size_t max_fault_directives{64U};
    };

    // A directive applies once to the next accepted send from its source.
    // Duplicate means one additional copy. Delay differences provide explicit
    // deterministic reordering. Drop takes precedence over the other fields.
    struct FaultDirective {
        bool drop{false};
        bool duplicate{false};
        std::optional<std::size_t> corrupt_byte_index;
        std::uint8_t corrupt_xor_mask{0x01U};
        Tick delay_ticks{0U};
    };

    struct Statistics {
        std::uint64_t accepted_sends{0U};
        std::uint64_t would_block_sends{0U};
        std::uint64_t disconnected_sends{0U};
        std::uint64_t closed_sends{0U};
        std::uint64_t invalid_sends{0U};
        std::uint64_t received_datagrams{0U};
        std::uint64_t delivered_datagrams{0U};
        std::uint64_t fault_drops{0U};
        std::uint64_t duplicate_copies{0U};
        std::uint64_t corrupted_copies{0U};
        std::uint64_t inbound_full_drops{0U};
        std::uint64_t disconnected_drops{0U};
        std::size_t scheduled_high_watermark{0U};
        std::array<std::size_t, 2U> inbound_high_watermarks{};
    };

    class Endpoint final : public ports::IDatagramTransport {
    public:
        std::size_t maximum_datagram_size() const noexcept override;
        ports::DatagramSendStatus try_send(
            const std::uint8_t* data, std::size_t size) noexcept override;
        ports::DatagramReceiveResult try_receive(
            std::uint8_t* destination,
            std::size_t destination_capacity) noexcept override;
        bool connected() const noexcept override;
        bool closed() const noexcept override;
        void close() noexcept override;

        EndpointId id() const noexcept;

    private:
        friend class SimulatedRadioBus;
        Endpoint(SimulatedRadioBus& bus, EndpointId id) noexcept;

        SimulatedRadioBus& bus_;
        EndpointId id_;
    };

    explicit SimulatedRadioBus(Config config);

    SimulatedRadioBus(const SimulatedRadioBus&) = delete;
    SimulatedRadioBus& operator=(const SimulatedRadioBus&) = delete;
    SimulatedRadioBus(SimulatedRadioBus&&) = delete;
    SimulatedRadioBus& operator=(SimulatedRadioBus&&) = delete;

    bool valid() const noexcept;
    const Config& config() const noexcept;

    Endpoint& first_endpoint() noexcept;
    Endpoint& second_endpoint() noexcept;

    // Returns false for an invalid directive, a closed source, or a full
    // directive queue. Rejected directives never alter the queue.
    bool enqueue_fault(
        EndpointId source, FaultDirective directive) noexcept;

    // Disconnect is reversible and discards all in-flight and already queued
    // link data. Close is permanent and cannot be reversed.
    bool set_connected(EndpointId endpoint, bool connected) noexcept;

    // Advancing backwards is rejected without changing state.
    bool advance_to(Tick tick) noexcept;
    Tick current_tick() const noexcept;

    std::size_t scheduled_event_count() const noexcept;
    std::size_t inbound_datagram_count(EndpointId endpoint) const noexcept;
    std::size_t fault_directive_count(EndpointId endpoint) const noexcept;
    const Statistics& statistics() const noexcept;

private:
    struct StoredDatagram {
        std::array<std::uint8_t, ports::kMaximumDatagramBytes> bytes{};
        std::size_t size{0U};
    };

    struct ScheduledEvent {
        Tick delivery_tick{0U};
        std::uint64_t ordinal{0U};
        EndpointId source{EndpointId::First};
        EndpointId destination{EndpointId::Second};
        StoredDatagram datagram;
    };

    static bool valid_config(const Config& config) noexcept;
    static std::size_t index_of(EndpointId endpoint) noexcept;
    static EndpointId other_endpoint(EndpointId endpoint) noexcept;

    ports::DatagramSendStatus send_from(
        EndpointId source, const std::uint8_t* data,
        std::size_t size) noexcept;
    ports::DatagramReceiveResult receive_for(
        EndpointId destination, std::uint8_t* data,
        std::size_t capacity) noexcept;
    bool endpoint_connected(EndpointId endpoint) const noexcept;
    bool endpoint_closed(EndpointId endpoint) const noexcept;
    void close_endpoint(EndpointId endpoint) noexcept;
    void discard_link_data() noexcept;
    void deliver_due_events() noexcept;
    void schedule_event(ScheduledEvent event) noexcept;

    const Config config_;
    const bool valid_;
    Tick current_tick_{0U};
    std::uint64_t next_event_ordinal_{0U};
    std::array<bool, 2U> connected_{{true, true}};
    std::array<bool, 2U> closed_{{false, false}};
    std::array<std::vector<StoredDatagram>, 2U> inbound_;
    std::array<std::vector<FaultDirective>, 2U> fault_directives_;
    std::vector<ScheduledEvent> scheduled_events_;
    Statistics statistics_;
    Endpoint first_endpoint_;
    Endpoint second_endpoint_;
};

} // namespace lora::adapters::transport
