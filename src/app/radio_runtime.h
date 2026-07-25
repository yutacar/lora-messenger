/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "application/transmission_scheduler.h"
#include "persistent_session.h"
#include "ports/datagram_transport.h"
#include "ports/radio_policy.h"
#include "protocol/reassembler.h"

#include <cstddef>
#include <cstdint>

namespace app {

struct RadioRuntimeMetrics {
    std::uint64_t received_datagrams{0U};
    std::uint64_t receive_overflow{0U};
    std::uint64_t completed_posts{0U};
    std::uint64_t duplicate_posts{0U};
    std::uint64_t rejected_posts{0U};
    std::uint64_t broadcast_posts{0U};
    std::uint64_t failed_posts{0U};
};

// Single-threaded bridge between the Phase 4 protocol/scheduler, durable
// session, and the real radio ports. pump() is bounded and performs no waits.
class RadioRuntime {
public:
    RadioRuntime(
        lora::ports::IDatagramTransport& transport,
        lora::ports::IRadioPolicy& radio_policy,
        lora::ports::IRandomBytes& random) noexcept;
    ~RadioRuntime();

    RadioRuntime(const RadioRuntime&) = delete;
    RadioRuntime& operator=(const RadioRuntime&) = delete;

    bool ready() const noexcept;

    // Returns true when the durable model changed and the UI should refresh.
    bool pump(
        lora::ports::RadioTick now,
        PersistentSession& session) noexcept;
    void stop() noexcept;

    const RadioRuntimeMetrics& metrics() const noexcept;

private:
    bool enqueue_queued_posts(
        lora::ports::RadioTick now,
        PersistentSession& session) noexcept;
    bool pump_outbound(
        lora::ports::RadioTick now,
        PersistentSession& session) noexcept;
    bool pump_inbound(
        lora::ports::RadioTick now,
        PersistentSession& session) noexcept;

    lora::ports::IDatagramTransport& transport_;
    lora::ports::IRadioPolicy& radio_policy_;
    lora::application::TransmissionScheduler scheduler_;
    lora::protocol::InboundFrameQueue inbound_;
    bool stopped_{false};
    bool valid_{false};
    RadioRuntimeMetrics metrics_;
};

} // namespace app
