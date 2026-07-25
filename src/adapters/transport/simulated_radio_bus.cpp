/*
 * SPDX-License-Identifier: MIT
 */

#include "simulated_radio_bus.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace lora::adapters::transport {

namespace {

constexpr std::size_t kEndpointCount = 2U;

} // namespace

SimulatedRadioBus::Endpoint::Endpoint(
    SimulatedRadioBus& bus, EndpointId id) noexcept
    : bus_(bus), id_(id) {}

std::size_t
SimulatedRadioBus::Endpoint::maximum_datagram_size() const noexcept {
    return bus_.valid() ? bus_.config().mtu : 0U;
}

ports::DatagramSendStatus SimulatedRadioBus::Endpoint::try_send(
    const std::uint8_t* data, std::size_t size) noexcept {
    return bus_.send_from(id_, data, size);
}

ports::DatagramReceiveResult SimulatedRadioBus::Endpoint::try_receive(
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    return bus_.receive_for(id_, destination, destination_capacity);
}

bool SimulatedRadioBus::Endpoint::connected() const noexcept {
    return bus_.endpoint_connected(id_);
}

bool SimulatedRadioBus::Endpoint::closed() const noexcept {
    return bus_.endpoint_closed(id_);
}

void SimulatedRadioBus::Endpoint::close() noexcept {
    bus_.close_endpoint(id_);
}

SimulatedRadioBus::EndpointId
SimulatedRadioBus::Endpoint::id() const noexcept {
    return id_;
}

SimulatedRadioBus::SimulatedRadioBus(Config config)
    : config_(config),
      valid_(valid_config(config)),
      first_endpoint_(*this, EndpointId::First),
      second_endpoint_(*this, EndpointId::Second) {
    if (!valid_) {
        connected_ = {{false, false}};
        return;
    }

    scheduled_events_.reserve(config_.max_scheduled_events);
    for (auto& queue : inbound_) {
        queue.reserve(config_.max_inbound_datagrams);
    }
    for (auto& queue : fault_directives_) {
        queue.reserve(config_.max_fault_directives);
    }
}

bool SimulatedRadioBus::valid_config(const Config& config) noexcept {
    const auto valid_depth = [](std::size_t depth) {
        return depth > 0U && depth <= kMaximumQueueDepth;
    };
    return config.mtu >= ports::kMinimumDatagramMtu &&
           config.mtu <= ports::kMaximumDatagramBytes &&
           valid_depth(config.max_scheduled_events) &&
           valid_depth(config.max_inbound_datagrams) &&
           valid_depth(config.max_fault_directives);
}

std::size_t
SimulatedRadioBus::index_of(EndpointId endpoint) noexcept {
    switch (endpoint) {
        case EndpointId::First:
            return 0U;
        case EndpointId::Second:
            return 1U;
    }
    return kEndpointCount;
}

SimulatedRadioBus::EndpointId
SimulatedRadioBus::other_endpoint(EndpointId endpoint) noexcept {
    return endpoint == EndpointId::First ? EndpointId::Second
                                         : EndpointId::First;
}

bool SimulatedRadioBus::valid() const noexcept {
    return valid_;
}

const SimulatedRadioBus::Config&
SimulatedRadioBus::config() const noexcept {
    return config_;
}

SimulatedRadioBus::Endpoint&
SimulatedRadioBus::first_endpoint() noexcept {
    return first_endpoint_;
}

SimulatedRadioBus::Endpoint&
SimulatedRadioBus::second_endpoint() noexcept {
    return second_endpoint_;
}

bool SimulatedRadioBus::enqueue_fault(
    EndpointId source, FaultDirective directive) noexcept {
    const auto index = index_of(source);
    if (!valid_ || index >= kEndpointCount || closed_[index] ||
        fault_directives_[index].size() >=
            config_.max_fault_directives) {
        return false;
    }

    if (!directive.drop) {
        if (directive.duplicate &&
            config_.max_scheduled_events < 2U) {
            return false;
        }
        if (directive.corrupt_byte_index &&
            (*directive.corrupt_byte_index >= config_.mtu ||
             directive.corrupt_xor_mask == 0U)) {
            return false;
        }
        if (directive.delay_ticks >
            std::numeric_limits<Tick>::max() - current_tick_) {
            return false;
        }
    }

    fault_directives_[index].push_back(std::move(directive));
    return true;
}

bool SimulatedRadioBus::set_connected(
    EndpointId endpoint, bool connected) noexcept {
    const auto index = index_of(endpoint);
    if (!valid_ || index >= kEndpointCount || closed_[index]) {
        return false;
    }
    if (connected_[index] == connected) {
        return true;
    }

    connected_[index] = connected;
    if (!connected) {
        discard_link_data();
    }
    return true;
}

bool SimulatedRadioBus::advance_to(Tick tick) noexcept {
    if (!valid_ || tick < current_tick_) {
        return false;
    }
    current_tick_ = tick;
    deliver_due_events();
    return true;
}

SimulatedRadioBus::Tick
SimulatedRadioBus::current_tick() const noexcept {
    return current_tick_;
}

std::size_t
SimulatedRadioBus::scheduled_event_count() const noexcept {
    return scheduled_events_.size();
}

std::size_t SimulatedRadioBus::inbound_datagram_count(
    EndpointId endpoint) const noexcept {
    const auto index = index_of(endpoint);
    return index < kEndpointCount ? inbound_[index].size() : 0U;
}

std::size_t SimulatedRadioBus::fault_directive_count(
    EndpointId endpoint) const noexcept {
    const auto index = index_of(endpoint);
    return index < kEndpointCount
        ? fault_directives_[index].size()
        : 0U;
}

const SimulatedRadioBus::Statistics&
SimulatedRadioBus::statistics() const noexcept {
    return statistics_;
}

ports::DatagramSendStatus SimulatedRadioBus::send_from(
    EndpointId source, const std::uint8_t* data,
    std::size_t size) noexcept {
    const auto source_index = index_of(source);
    if (!valid_ || source_index >= kEndpointCount || data == nullptr ||
        size == 0U || size > config_.mtu ||
        size > ports::kMaximumDatagramBytes) {
        ++statistics_.invalid_sends;
        return ports::DatagramSendStatus::Invalid;
    }
    if (closed_[source_index]) {
        ++statistics_.closed_sends;
        return ports::DatagramSendStatus::Closed;
    }
    if (!endpoint_connected(source)) {
        ++statistics_.disconnected_sends;
        return ports::DatagramSendStatus::Disconnected;
    }

    const FaultDirective* directive =
        fault_directives_[source_index].empty()
            ? nullptr
            : &fault_directives_[source_index].front();
    const bool drop = directive != nullptr && directive->drop;
    const std::size_t copy_count =
        drop ? 0U
             : (directive != nullptr && directive->duplicate ? 2U : 1U);

    Tick delivery_tick = current_tick_;
    if (!drop && directive != nullptr) {
        if (directive->corrupt_byte_index &&
            (*directive->corrupt_byte_index >= size ||
             directive->corrupt_xor_mask == 0U)) {
            ++statistics_.invalid_sends;
            return ports::DatagramSendStatus::Invalid;
        }
        if (directive->delay_ticks >
            std::numeric_limits<Tick>::max() - current_tick_) {
            ++statistics_.invalid_sends;
            return ports::DatagramSendStatus::Invalid;
        }
        delivery_tick += directive->delay_ticks;
    }

    if (copy_count >
        config_.max_scheduled_events - scheduled_events_.size()) {
        ++statistics_.would_block_sends;
        return ports::DatagramSendStatus::WouldBlock;
    }
    if (copy_count >
        std::numeric_limits<std::uint64_t>::max() -
            next_event_ordinal_) {
        ++statistics_.invalid_sends;
        return ports::DatagramSendStatus::Invalid;
    }

    FaultDirective accepted_directive;
    const bool had_directive = directive != nullptr;
    if (had_directive) {
        accepted_directive = *directive;
        fault_directives_[source_index].erase(
            fault_directives_[source_index].begin());
    }

    ++statistics_.accepted_sends;
    if (drop) {
        ++statistics_.fault_drops;
        return ports::DatagramSendStatus::Accepted;
    }

    StoredDatagram datagram;
    std::copy_n(data, size, datagram.bytes.begin());
    datagram.size = size;
    if (had_directive &&
        accepted_directive.corrupt_byte_index) {
        datagram.bytes[*accepted_directive.corrupt_byte_index] ^=
            accepted_directive.corrupt_xor_mask;
    }

    if (copy_count == 2U) {
        ++statistics_.duplicate_copies;
    }
    if (had_directive &&
        accepted_directive.corrupt_byte_index) {
        statistics_.corrupted_copies += copy_count;
    }

    for (std::size_t copy = 0U; copy < copy_count; ++copy) {
        ScheduledEvent event;
        event.delivery_tick = delivery_tick;
        event.ordinal = next_event_ordinal_++;
        event.source = source;
        event.destination = other_endpoint(source);
        event.datagram = datagram;
        schedule_event(std::move(event));
    }
    deliver_due_events();
    return ports::DatagramSendStatus::Accepted;
}

ports::DatagramReceiveResult SimulatedRadioBus::receive_for(
    EndpointId destination, std::uint8_t* data,
    std::size_t capacity) noexcept {
    const auto destination_index = index_of(destination);
    if (!valid_ || destination_index >= kEndpointCount ||
        data == nullptr || capacity == 0U) {
        return {ports::DatagramReceiveStatus::Invalid, 0U};
    }
    if (closed_[destination_index]) {
        return {ports::DatagramReceiveStatus::Closed, 0U};
    }
    if (!endpoint_connected(destination)) {
        return {ports::DatagramReceiveStatus::Disconnected, 0U};
    }

    auto& inbound = inbound_[destination_index];
    if (inbound.empty()) {
        return {ports::DatagramReceiveStatus::WouldBlock, 0U};
    }
    if (capacity < inbound.front().size) {
        return {ports::DatagramReceiveStatus::Invalid, 0U};
    }

    const auto datagram = inbound.front();
    std::copy_n(datagram.bytes.begin(), datagram.size, data);
    inbound.erase(inbound.begin());
    ++statistics_.received_datagrams;
    return {ports::DatagramReceiveStatus::Received, datagram.size};
}

bool SimulatedRadioBus::endpoint_connected(
    EndpointId endpoint) const noexcept {
    const auto index = index_of(endpoint);
    if (!valid_ || index >= kEndpointCount) {
        return false;
    }
    const auto peer_index = index_of(other_endpoint(endpoint));
    return connected_[index] && connected_[peer_index] &&
           !closed_[index] && !closed_[peer_index];
}

bool SimulatedRadioBus::endpoint_closed(
    EndpointId endpoint) const noexcept {
    const auto index = index_of(endpoint);
    return !valid_ || index >= kEndpointCount || closed_[index];
}

void SimulatedRadioBus::close_endpoint(
    EndpointId endpoint) noexcept {
    const auto index = index_of(endpoint);
    if (!valid_ || index >= kEndpointCount || closed_[index]) {
        return;
    }
    closed_[index] = true;
    connected_[index] = false;
    fault_directives_[index].clear();
    discard_link_data();
}

void SimulatedRadioBus::discard_link_data() noexcept {
    std::uint64_t discarded =
        static_cast<std::uint64_t>(scheduled_events_.size());
    scheduled_events_.clear();
    for (auto& queue : inbound_) {
        discarded += static_cast<std::uint64_t>(queue.size());
        queue.clear();
    }
    statistics_.disconnected_drops += discarded;
}

void SimulatedRadioBus::deliver_due_events() noexcept {
    while (!scheduled_events_.empty() &&
           scheduled_events_.front().delivery_tick <= current_tick_) {
        const auto event = scheduled_events_.front();
        scheduled_events_.erase(scheduled_events_.begin());

        if (!endpoint_connected(event.source) ||
            !endpoint_connected(event.destination)) {
            ++statistics_.disconnected_drops;
            continue;
        }

        const auto destination_index = index_of(event.destination);
        auto& inbound = inbound_[destination_index];
        if (inbound.size() >= config_.max_inbound_datagrams) {
            ++statistics_.inbound_full_drops;
            continue;
        }
        inbound.push_back(event.datagram);
        ++statistics_.delivered_datagrams;
        statistics_.inbound_high_watermarks[destination_index] =
            std::max(statistics_.inbound_high_watermarks[destination_index],
                     inbound.size());
    }
}

void SimulatedRadioBus::schedule_event(
    ScheduledEvent event) noexcept {
    const auto position = std::upper_bound(
        scheduled_events_.begin(), scheduled_events_.end(), event,
        [](const ScheduledEvent& candidate,
           const ScheduledEvent& existing) {
            if (candidate.delivery_tick != existing.delivery_tick) {
                return candidate.delivery_tick <
                       existing.delivery_tick;
            }
            return candidate.ordinal < existing.ordinal;
        });
    scheduled_events_.insert(position, std::move(event));
    statistics_.scheduled_high_watermark =
        std::max(statistics_.scheduled_high_watermark,
                 scheduled_events_.size());
}

} // namespace lora::adapters::transport
