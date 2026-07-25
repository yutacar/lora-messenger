/*
 * SPDX-License-Identifier: MIT
 */

#include "phase4_pipe_protocol.h"

#include "adapters/transport/simulated_radio_bus.h"
#include "application/transmission_scheduler.h"
#include "protocol/dedupe_window.h"
#include "protocol/fragmenter.h"
#include "protocol/limits.h"
#include "protocol/reassembler.h"

#include "../unit/test_support.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using lora::adapters::transport::SimulatedRadioBus;
using lora::application::OutboundTerminalState;
using lora::application::SchedulerError;
using lora::application::TransmissionScheduler;
using lora::protocol::Bytes;
using lora::protocol::DedupeClassification;
using lora::protocol::DedupeError;
using lora::protocol::IngressEnqueueStatus;
using lora::protocol::IngressProcessStatus;
using lora::test::phase4_pipe::Response;

constexpr std::array<std::size_t, 5U> kGateMtus{
    48U, 51U, 64U, 128U, 255U};
constexpr std::uint32_t kStressSeedCount = 10'000U;
constexpr std::uint64_t kSimulationTickStep = 50U;
constexpr std::uint64_t kSimulationTickLimit = 20'000U;

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

class DeterministicRandom final : public lora::ports::IRandomBytes {
public:
    explicit DeterministicRandom(std::uint64_t seed) noexcept
        : state_(seed ^ 0x6a09e667f3bcc909ULL) {}

    bool fill(std::uint8_t* destination,
              std::size_t size) noexcept override {
        if (destination == nullptr && size != 0U) {
            return false;
        }
        std::size_t offset = 0U;
        while (offset < size) {
            const std::uint64_t value = splitmix64(state_);
            for (std::size_t byte = 0U;
                 byte < sizeof(value) && offset < size;
                 ++byte, ++offset) {
                destination[offset] = static_cast<std::uint8_t>(
                    value >> (byte * 8U));
            }
        }
        return true;
    }

private:
    std::uint64_t state_;
};

class AllowAllRadioPolicy final : public lora::ports::IRadioPolicy {
public:
    lora::ports::RadioTransmitDecision evaluate_transmit(
        lora::ports::RadioTick now,
        std::size_t) noexcept override {
        return {lora::ports::RadioTransmitStatus::Allowed, now};
    }

    void record_transmit(
        lora::ports::RadioTick,
        std::size_t) noexcept override {}
};

lora::core::Uuid deterministic_uuid(std::uint64_t seed,
                                    std::uint64_t domain) noexcept {
    std::uint64_t state =
        seed ^ (domain * 0xd1342543de82ef95ULL);
    const std::uint64_t first = splitmix64(state);
    const std::uint64_t second = splitmix64(state);
    lora::core::Uuid::Bytes bytes{};
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[index] =
            static_cast<std::uint8_t>(first >> (index * 8U));
        bytes[index + 8U] =
            static_cast<std::uint8_t>(second >> (index * 8U));
    }
    bytes[6] =
        static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] =
        static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    return lora::core::Uuid::from_bytes(bytes);
}

std::optional<lora::model::PostPayload> maximum_post(
    std::uint32_t seed, std::uint8_t direction) {
    const std::uint64_t base =
        static_cast<std::uint64_t>(seed) * 32U +
        static_cast<std::uint64_t>(direction) * 16U;
    lora::model::PostPayloadInput input;
    input.message_id = deterministic_uuid(base, 1U);
    input.sender_id = deterministic_uuid(base, 2U);
    input.sender_sequence =
        static_cast<std::uint64_t>(seed) * 2U + direction + 1U;

    input.sender_user_id =
        direction == 0U ? "alpha-node-" : "bravo-node-";
    input.sender_user_id += std::to_string(seed);
    input.sender_user_id.resize(24U, direction == 0U ? 'a' : 'b');

    input.body = "phase4-seed-" + std::to_string(seed) +
                 (direction == 0U ? "-alpha-" : "-bravo-");
    input.body.resize(160U, direction == 0U ? 'A' : 'B');
    for (std::uint64_t mention = 0U; mention < 4U; ++mention) {
        input.mentions.push_back(
            deterministic_uuid(base, 10U + mention));
    }
    input.reply_to = deterministic_uuid(base, 20U);
    input.sender_time =
        direction == 0U
            ? static_cast<lora::ports::UnixSeconds>(seed)
            : -static_cast<lora::ports::UnixSeconds>(seed) - 1;

    auto created =
        lora::model::PostPayload::create(std::move(input));
    if (!created) {
        return std::nullopt;
    }
    return std::optional<lora::model::PostPayload>(
        std::move(created).value());
}

enum class FaultProfile : std::uint8_t {
    Clean,
    Loss,
    Duplicate,
    Corrupt,
    Delay,
    Reorder,
    DisconnectReconnect,
    Mixed,
    PermanentLoss,
};

constexpr std::array<FaultProfile, 9U> kFaultProfiles{
    FaultProfile::Clean,
    FaultProfile::Loss,
    FaultProfile::Duplicate,
    FaultProfile::Corrupt,
    FaultProfile::Delay,
    FaultProfile::Reorder,
    FaultProfile::DisconnectReconnect,
    FaultProfile::Mixed,
    FaultProfile::PermanentLoss,
};

std::string_view fault_profile_name(FaultProfile profile) noexcept {
    switch (profile) {
        case FaultProfile::Clean:
            return "clean";
        case FaultProfile::Loss:
            return "loss";
        case FaultProfile::Duplicate:
            return "duplicate";
        case FaultProfile::Corrupt:
            return "corrupt";
        case FaultProfile::Delay:
            return "delay";
        case FaultProfile::Reorder:
            return "reorder";
        case FaultProfile::DisconnectReconnect:
            return "disconnect-reconnect";
        case FaultProfile::Mixed:
            return "mixed";
        case FaultProfile::PermanentLoss:
            return "permanent-loss";
    }
    return "unknown";
}

bool enqueue_fault_script(
    SimulatedRadioBus& bus, SimulatedRadioBus::EndpointId source,
    FaultProfile profile, std::size_t fragment_count,
    std::uint32_t seed) {
    if (profile == FaultProfile::Clean ||
        profile == FaultProfile::DisconnectReconnect) {
        return true;
    }

    const std::size_t passes =
        profile == FaultProfile::PermanentLoss ? 2U : 1U;
    const std::size_t chosen =
        static_cast<std::size_t>(seed) % fragment_count;
    for (std::size_t pass = 0U; pass < passes; ++pass) {
        for (std::size_t index = 0U; index < fragment_count;
             ++index) {
            SimulatedRadioBus::FaultDirective directive;
            switch (profile) {
                case FaultProfile::Loss:
                    directive.drop = index == chosen;
                    break;
                case FaultProfile::Duplicate:
                    directive.duplicate = index == chosen;
                    break;
                case FaultProfile::Corrupt:
                    if (index == chosen) {
                        directive.corrupt_byte_index = 0U;
                    }
                    break;
                case FaultProfile::Delay:
                    directive.delay_ticks = 250U;
                    break;
                case FaultProfile::Reorder:
                    directive.delay_ticks =
                        static_cast<std::uint64_t>(
                            fragment_count - index) *
                        100U;
                    break;
                case FaultProfile::Mixed:
                    if (index == 0U) {
                        directive.drop = true;
                    } else {
                        directive.delay_ticks =
                            static_cast<std::uint64_t>(
                                fragment_count - index) *
                            100U;
                        if (index == 1U) {
                            directive.duplicate = true;
                            directive.corrupt_byte_index = 0U;
                        }
                    }
                    break;
                case FaultProfile::PermanentLoss:
                    directive.drop = index == chosen;
                    break;
                case FaultProfile::Clean:
                case FaultProfile::DisconnectReconnect:
                    break;
            }
            if (!bus.enqueue_fault(source, directive)) {
                return false;
            }
        }
    }
    return true;
}

class SimulationNode {
public:
    SimulationNode(std::uint64_t random_seed, std::size_t mtu)
        : random_(random_seed),
          scheduler_(random_),
          mtu_(mtu),
          inbound_(mtu),
          dedupe_(64U) {}

    std::optional<std::string> drain(
        lora::ports::IDatagramTransport& transport,
        std::uint64_t tick) {
        std::array<std::uint8_t,
                   lora::protocol::kMaximumTransportMtu>
            datagram{};
        for (;;) {
            const auto received = transport.try_receive(
                datagram.data(), datagram.size());
            switch (received.status) {
                case lora::ports::DatagramReceiveStatus::WouldBlock:
                case lora::ports::DatagramReceiveStatus::Disconnected:
                    return std::nullopt;
                case lora::ports::DatagramReceiveStatus::Received:
                    break;
                case lora::ports::DatagramReceiveStatus::Closed:
                    return std::string("transport closed");
                case lora::ports::DatagramReceiveStatus::Invalid:
                    return std::string("transport receive invalid");
            }
            if (received.size > datagram.size()) {
                return std::string("transport exceeded receive bound");
            }
            auto decoded_frame = lora::protocol::decode_frame(
                datagram.data(), received.size, mtu_);
            if (decoded_frame) {
                const auto fragment_index =
                    decoded_frame.value().fragment_index;
                if (!unique_fragments_[fragment_index]) {
                    if (last_unique_fragment_ &&
                        fragment_index < *last_unique_fragment_) {
                        observed_fragment_reordering_ = true;
                    }
                    unique_fragments_[fragment_index] = true;
                    last_unique_fragment_ = fragment_index;
                }
            }
            if (inbound_.enqueue(datagram.data(), received.size, tick) !=
                IngressEnqueueStatus::Accepted) {
                return std::string("inbound queue rejected bus datagram");
            }

            auto processed = inbound_.process_next(tick);
            switch (processed.status) {
                case IngressProcessStatus::FragmentAccepted:
                case IngressProcessStatus::DuplicateFragment:
                case IngressProcessStatus::RecentDuplicate:
                case IngressProcessStatus::Malformed:
                    break;
                case IngressProcessStatus::Complete: {
                    if (!processed.completed) {
                        return std::string(
                            "complete status without completed post");
                    }
                    auto error = commit(*processed.completed, tick);
                    if (error) {
                        return error;
                    }
                    break;
                }
                case IngressProcessStatus::CapacityExceeded:
                    return std::string(
                        "reassembly capacity unexpectedly exceeded");
                case IngressProcessStatus::Conflict:
                    return std::string("reassembly conflict");
                case IngressProcessStatus::Quarantined:
                    return std::string("reassembly key quarantined");
                case IngressProcessStatus::InvalidPost:
                    return std::string("reassembly produced invalid post");
                case IngressProcessStatus::Stale:
                    return std::string("fresh bus datagram was stale");
                case IngressProcessStatus::Empty:
                case IngressProcessStatus::Closed:
                    return std::string(
                        "inbound queue did not process enqueued datagram");
            }
        }
    }

    void expire(std::uint64_t tick) {
        static_cast<void>(inbound_.process_next(tick));
    }

    TransmissionScheduler& scheduler() noexcept { return scheduler_; }

    const std::vector<Bytes>& commits() const noexcept {
        return commits_;
    }

    bool observed_fragment_reordering() const noexcept {
        return observed_fragment_reordering_;
    }

    const lora::protocol::IngressMetrics& inbound_metrics() const noexcept {
        return inbound_.metrics();
    }

    const lora::protocol::MessageDedupeWindow& dedupe() const noexcept {
        return dedupe_;
    }

private:
    std::optional<std::string> commit(
        const lora::protocol::CompletedPost& completed,
        std::uint64_t tick) {
        switch (dedupe_.classify(completed.post.message_id(),
                                 completed.canonical_post)) {
            case DedupeClassification::New:
                if (dedupe_.remember(completed.post,
                                     completed.canonical_post) !=
                    DedupeError::None) {
                    return std::string("dedupe remember failed");
                }
                if (std::find(
                        commits_.begin(), commits_.end(),
                        completed.canonical_post) != commits_.end()) {
                    return std::string("duplicate durable commit");
                }
                commits_.push_back(completed.canonical_post);
                inbound_.mark_committed(
                    completed.canonical_frames, tick);
                return std::nullopt;
            case DedupeClassification::Duplicate:
                inbound_.mark_committed(
                    completed.canonical_frames, tick);
                return std::nullopt;
            case DedupeClassification::Conflict:
                return std::string("dedupe conflict");
            case DedupeClassification::Invalid:
                return std::string("dedupe classified invalid");
        }
        return std::string("unreachable dedupe classification");
    }

    DeterministicRandom random_;
    TransmissionScheduler scheduler_;
    std::size_t mtu_;
    lora::protocol::InboundFrameQueue inbound_;
    lora::protocol::MessageDedupeWindow dedupe_;
    std::array<bool, lora::protocol::kMaximumFragments>
        unique_fragments_{};
    std::optional<std::uint8_t> last_unique_fragment_;
    bool observed_fragment_reordering_{false};
    std::vector<Bytes> commits_;
};

struct SimulationResult {
    bool passed{false};
    std::string detail;
};

SimulationResult simulation_failure(
    std::string detail) {
    return {false, std::move(detail)};
}

bool contains_exactly(
    const std::vector<Bytes>& commits,
    const Bytes& expected) {
    return commits.size() == 1U && commits.front() == expected;
}

SimulationResult run_simulation(std::uint32_t seed,
                                std::size_t mtu,
                                FaultProfile profile) {
    auto alpha_post = maximum_post(seed, 0U);
    auto bravo_post = maximum_post(seed, 1U);
    if (!alpha_post || !bravo_post) {
        return simulation_failure("could not create maximum post");
    }
    auto alpha_encoded =
        lora::protocol::encode_post(*alpha_post);
    auto bravo_encoded =
        lora::protocol::encode_post(*bravo_post);
    if (!alpha_encoded || !bravo_encoded ||
        alpha_encoded.value().size() !=
            lora::protocol::kMaximumEncodedPostBytes ||
        bravo_encoded.value().size() !=
            lora::protocol::kMaximumEncodedPostBytes) {
        return simulation_failure(
            "stress post did not cover maximum encoded size");
    }
    auto alpha_frames =
        lora::protocol::fragment_post(*alpha_post, mtu);
    auto bravo_frames =
        lora::protocol::fragment_post(*bravo_post, mtu);
    if (!alpha_frames || !bravo_frames ||
        alpha_frames.value().empty() ||
        alpha_frames.value().size() !=
            bravo_frames.value().size()) {
        return simulation_failure("fragmentation failed");
    }

    SimulatedRadioBus::Config config;
    config.mtu = mtu;
    config.max_scheduled_events = 128U;
    config.max_inbound_datagrams = 64U;
    config.max_fault_directives = 64U;
    SimulatedRadioBus bus(config);
    if (!bus.valid()) {
        return simulation_failure("simulated bus config invalid");
    }
    if (!enqueue_fault_script(
            bus, SimulatedRadioBus::EndpointId::First, profile,
            alpha_frames.value().size(), seed) ||
        !enqueue_fault_script(
            bus, SimulatedRadioBus::EndpointId::Second,
            profile == FaultProfile::PermanentLoss
                ? FaultProfile::Clean
                : profile,
            bravo_frames.value().size(), seed + 1U)) {
        return simulation_failure("fault script exceeded bound");
    }

    bool link_reconnected =
        profile != FaultProfile::DisconnectReconnect;
    if (!link_reconnected &&
        !bus.set_connected(
            SimulatedRadioBus::EndpointId::Second, false)) {
        return simulation_failure("could not disconnect link");
    }

    SimulationNode alpha(
        static_cast<std::uint64_t>(seed) * 2U + 11U, mtu);
    SimulationNode bravo(
        static_cast<std::uint64_t>(seed) * 2U + 12U, mtu);
    AllowAllRadioPolicy alpha_policy;
    AllowAllRadioPolicy bravo_policy;
    if (alpha.scheduler().enqueue(*alpha_post, mtu, 0U) !=
            SchedulerError::None ||
        bravo.scheduler().enqueue(*bravo_post, mtu, 0U) !=
            SchedulerError::None) {
        return simulation_failure("scheduler enqueue failed");
    }

    std::size_t alpha_broadcasts = 0U;
    std::size_t bravo_broadcasts = 0U;
    bool settled = false;
    std::uint64_t final_tick = 0U;
    for (std::uint64_t tick = 0U; tick <= kSimulationTickLimit;
         tick += kSimulationTickStep) {
        final_tick = tick;
        if (!bus.advance_to(tick)) {
            return simulation_failure("bus rejected monotonic tick");
        }

        const auto alpha_event = alpha.scheduler().pump(
            tick, bus.first_endpoint(), alpha_policy);
        const auto bravo_event = bravo.scheduler().pump(
            tick, bus.second_endpoint(), bravo_policy);
        if (alpha_event) {
            if (alpha_event->state != OutboundTerminalState::Broadcast ||
                alpha_event->message_id != alpha_post->message_id()) {
                return simulation_failure(
                    "alpha scheduler terminal failure");
            }
            ++alpha_broadcasts;
        }
        if (bravo_event) {
            if (bravo_event->state != OutboundTerminalState::Broadcast ||
                bravo_event->message_id != bravo_post->message_id()) {
                return simulation_failure(
                    "bravo scheduler terminal failure");
            }
            ++bravo_broadcasts;
        }

        if (!link_reconnected &&
            (alpha.scheduler().metrics().backpressure_events != 0U ||
             bravo.scheduler().metrics().backpressure_events != 0U)) {
            if (!bus.set_connected(
                    SimulatedRadioBus::EndpointId::Second, true)) {
                return simulation_failure("could not reconnect link");
            }
            link_reconnected = true;
        }

        if (auto error =
                alpha.drain(bus.first_endpoint(), tick)) {
            return simulation_failure("alpha receive: " + *error);
        }
        if (auto error =
                bravo.drain(bus.second_endpoint(), tick)) {
            return simulation_failure("bravo receive: " + *error);
        }

        if (alpha.scheduler().empty() &&
            bravo.scheduler().empty() &&
            bus.scheduled_event_count() == 0U &&
            bus.inbound_datagram_count(
                SimulatedRadioBus::EndpointId::First) == 0U &&
            bus.inbound_datagram_count(
                SimulatedRadioBus::EndpointId::Second) == 0U &&
            bus.fault_directive_count(
                SimulatedRadioBus::EndpointId::First) == 0U &&
            bus.fault_directive_count(
                SimulatedRadioBus::EndpointId::Second) == 0U) {
            settled = true;
            break;
        }
    }
    if (!settled) {
        return simulation_failure("simulation did not settle");
    }

    alpha.expire(final_tick +
                 lora::protocol::kReassemblyTimeoutTicks);
    bravo.expire(final_tick +
                 lora::protocol::kReassemblyTimeoutTicks);

    if (alpha_broadcasts != 1U || bravo_broadcasts != 1U ||
        alpha.scheduler().metrics().failed_messages != 0U ||
        bravo.scheduler().metrics().failed_messages != 0U) {
        return simulation_failure(
            "scheduler did not reach one local broadcast per post");
    }

    if (profile != FaultProfile::PermanentLoss) {
        if (!contains_exactly(alpha.commits(),
                              bravo_encoded.value()) ||
            !contains_exactly(bravo.commits(),
                              alpha_encoded.value())) {
            return simulation_failure(
                "deliverable profile did not fully converge");
        }
    } else {
        if (alpha.commits().size() > 1U ||
            bravo.commits().size() > 1U ||
            (!alpha.commits().empty() &&
             alpha.commits().front() !=
                 bravo_encoded.value()) ||
            (!bravo.commits().empty() &&
             bravo.commits().front() !=
                 alpha_encoded.value()) ||
            alpha.commits().size() + bravo.commits().size() != 1U) {
            return simulation_failure(
                "permanent loss produced an invalid subset");
        }
    }

    const auto& statistics = bus.statistics();
    if (statistics.scheduled_high_watermark >
            config.max_scheduled_events ||
        statistics.inbound_high_watermarks[0] >
            config.max_inbound_datagrams ||
        statistics.inbound_high_watermarks[1] >
            config.max_inbound_datagrams ||
        alpha.inbound_metrics().queue_high_water >
            lora::protocol::kMaximumInboundFrames ||
        bravo.inbound_metrics().queue_high_water >
            lora::protocol::kMaximumInboundFrames ||
        alpha.inbound_metrics().reassembly_high_water >
            lora::protocol::kMaximumReassemblies ||
        bravo.inbound_metrics().reassembly_high_water >
            lora::protocol::kMaximumReassemblies ||
        alpha.scheduler().metrics().high_water_messages >
            lora::application::kMaximumOutboundMessages ||
        bravo.scheduler().metrics().high_water_messages >
            lora::application::kMaximumOutboundMessages ||
        alpha.dedupe().size() > alpha.dedupe().capacity() ||
        bravo.dedupe().size() > bravo.dedupe().capacity()) {
        return simulation_failure("a bounded structure exceeded capacity");
    }

    switch (profile) {
        case FaultProfile::Clean:
            if (statistics.fault_drops != 0U ||
                statistics.duplicate_copies != 0U ||
                statistics.corrupted_copies != 0U) {
                return simulation_failure(
                    "clean profile injected a fault");
            }
            break;
        case FaultProfile::Loss:
            if (statistics.fault_drops < 2U) {
                return simulation_failure(
                    "loss profile did not drop both directions");
            }
            break;
        case FaultProfile::Duplicate:
            if (statistics.duplicate_copies < 2U ||
                alpha.inbound_metrics().duplicate_frames == 0U ||
                bravo.inbound_metrics().duplicate_frames == 0U) {
                return simulation_failure(
                    "duplicate profile was not observed end-to-end");
            }
            break;
        case FaultProfile::Corrupt:
            if (statistics.corrupted_copies < 2U ||
                alpha.inbound_metrics().malformed_frames == 0U ||
                bravo.inbound_metrics().malformed_frames == 0U) {
                return simulation_failure(
                    "corruption was not rejected by both receivers");
            }
            break;
        case FaultProfile::Delay:
            if (statistics.scheduled_high_watermark == 0U) {
                return simulation_failure(
                    "delayed profile did not schedule datagrams");
            }
            break;
        case FaultProfile::Reorder:
            if (statistics.scheduled_high_watermark == 0U ||
                !alpha.observed_fragment_reordering() ||
                !bravo.observed_fragment_reordering()) {
                return simulation_failure(
                    "reorder profile did not reverse unique fragments");
            }
            break;
        case FaultProfile::DisconnectReconnect:
            if (!link_reconnected ||
                statistics.disconnected_sends == 0U ||
                (alpha.scheduler().metrics().backpressure_events == 0U &&
                 bravo.scheduler().metrics().backpressure_events == 0U)) {
                return simulation_failure(
                    "disconnect/reconnect did not exercise retry");
            }
            break;
        case FaultProfile::Mixed:
            if (statistics.fault_drops < 2U ||
                statistics.duplicate_copies < 2U ||
                statistics.corrupted_copies < 2U) {
                return simulation_failure(
                    "mixed profile did not inject its fault set");
            }
            break;
        case FaultProfile::PermanentLoss:
            if (statistics.fault_drops < 2U) {
                return simulation_failure(
                    "permanent loss did not span both offers");
            }
            break;
    }
    return {true, {}};
}

bool set_close_on_exec(int descriptor) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return false;
    }
    int result = -1;
    do {
        result =
            ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ~ChildProcess() {
        close_input();
        close_output();
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGKILL));
            int status = 0;
            if (!wait_for_exit(2'000, status)) {
                static_cast<void>(
                    ::waitpid(pid_, &status, WNOHANG));
                pid_ = -1;
            }
        }
    }

    bool spawn(const std::filesystem::path& executable,
               std::size_t mtu) {
        if (pid_ > 0 || input_ >= 0 || output_ >= 0) {
            return false;
        }
        int child_input[2]{-1, -1};
        int child_output[2]{-1, -1};
        if (::pipe(child_input) != 0) {
            return false;
        }
        if (::pipe(child_output) != 0) {
            static_cast<void>(::close(child_input[0]));
            static_cast<void>(::close(child_input[1]));
            return false;
        }
        if (!set_close_on_exec(child_input[0]) ||
            !set_close_on_exec(child_input[1]) ||
            !set_close_on_exec(child_output[0]) ||
            !set_close_on_exec(child_output[1])) {
            static_cast<void>(::close(child_input[0]));
            static_cast<void>(::close(child_input[1]));
            static_cast<void>(::close(child_output[0]));
            static_cast<void>(::close(child_output[1]));
            return false;
        }

        const std::string path = executable.string();
        const std::string mtu_text = std::to_string(mtu);
        const pid_t child = ::fork();
        if (child < 0) {
            static_cast<void>(::close(child_input[0]));
            static_cast<void>(::close(child_input[1]));
            static_cast<void>(::close(child_output[0]));
            static_cast<void>(::close(child_output[1]));
            return false;
        }
        if (child == 0) {
            if (::dup2(child_input[0], STDIN_FILENO) < 0 ||
                ::dup2(child_output[1], STDOUT_FILENO) < 0) {
                ::_exit(126);
            }
            static_cast<void>(::close(child_input[0]));
            static_cast<void>(::close(child_input[1]));
            static_cast<void>(::close(child_output[0]));
            static_cast<void>(::close(child_output[1]));
            ::execl(path.c_str(), path.c_str(), "--mtu",
                    mtu_text.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }

        static_cast<void>(::close(child_input[0]));
        static_cast<void>(::close(child_output[1]));
        pid_ = child;
        input_ = child_input[1];
        output_ = child_output[0];
        return true;
    }

    bool send_record(const Bytes& datagram,
                     int timeout_milliseconds = 1'000) {
        if (datagram.empty() ||
            datagram.size() >
                lora::test::phase4_pipe::kMaximumRecordBytes) {
            return false;
        }
        std::array<std::uint8_t, 2U> prefix{
            static_cast<std::uint8_t>(datagram.size() >> 8U),
            static_cast<std::uint8_t>(datagram.size())};
        return write_all(prefix.data(), prefix.size(),
                         timeout_milliseconds) &&
               write_all(datagram.data(), datagram.size(),
                         timeout_milliseconds);
    }

    bool request_shutdown(int timeout_milliseconds = 1'000) {
        const std::array<std::uint8_t, 2U> prefix{0U, 0U};
        return write_all(prefix.data(), prefix.size(),
                         timeout_milliseconds);
    }

    enum class ReadResult {
        Received,
        TimedOut,
        Closed,
        Failed,
    };

    ReadResult read_response(Response& response,
                             int timeout_milliseconds) {
        Bytes discarded;
        return read_response(
            response, discarded, timeout_milliseconds);
    }

    ReadResult read_response(Response& response,
                             Bytes& canonical_post,
                             int timeout_milliseconds) {
        canonical_post.clear();
        std::array<std::uint8_t,
                   lora::test::phase4_pipe::kResponseHeaderBytes>
            header{};
        const auto header_result = read_all(
            header.data(), header.size(), timeout_milliseconds);
        if (header_result != ReadResult::Received) {
            return header_result;
        }
        const std::size_t payload_size =
            (static_cast<std::size_t>(header[1]) << 8U) |
            static_cast<std::size_t>(header[2]);
        if (payload_size >
            lora::protocol::kMaximumEncodedPostBytes) {
            return ReadResult::Failed;
        }
        canonical_post.resize(payload_size);
        const auto payload_result =
            read_all(canonical_post.data(), canonical_post.size(),
                     timeout_milliseconds);
        if (payload_result != ReadResult::Received) {
            canonical_post.clear();
            return payload_result;
        }
        response = static_cast<Response>(header[0]);
        return ReadResult::Received;
    }

    void close_input() noexcept {
        if (input_ >= 0) {
            static_cast<void>(::close(input_));
            input_ = -1;
        }
    }

    bool wait_for_exit(int timeout_milliseconds, int& status) {
        if (pid_ <= 0) {
            return false;
        }
        const int iterations =
            std::max(1, timeout_milliseconds / 5);
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return true;
            }
            if (result < 0 && errno != EINTR) {
                return false;
            }
            static_cast<void>(::usleep(5'000U));
        }
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return true;
        }
        return false;
    }

private:
    ReadResult read_all(std::uint8_t* data, std::size_t size,
                        int timeout_milliseconds) {
        if (output_ < 0) {
            return ReadResult::Closed;
        }
        std::size_t offset = 0U;
        while (offset < size) {
            pollfd descriptor{output_, POLLIN, 0};
            int ready = 0;
            do {
                ready =
                    ::poll(&descriptor, 1, timeout_milliseconds);
            } while (ready < 0 && errno == EINTR);
            if (ready == 0) {
                return ReadResult::TimedOut;
            }
            if (ready < 0) {
                return ReadResult::Failed;
            }
            ssize_t count = 0;
            do {
                count =
                    ::read(output_, data + offset, size - offset);
            } while (count < 0 && errno == EINTR);
            if (count == 0) {
                return offset == 0U ? ReadResult::Closed
                                    : ReadResult::Failed;
            }
            if (count < 0) {
                return ReadResult::Failed;
            }
            offset += static_cast<std::size_t>(count);
        }
        return ReadResult::Received;
    }

    bool write_all(const std::uint8_t* data, std::size_t size,
                   int timeout_milliseconds) {
        if (input_ < 0) {
            return false;
        }
        std::size_t offset = 0U;
        while (offset < size) {
            pollfd descriptor{input_, POLLOUT, 0};
            int ready = 0;
            do {
                ready =
                    ::poll(&descriptor, 1, timeout_milliseconds);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0) {
                return false;
            }
            ssize_t count = 0;
            do {
                count =
                    ::write(input_, data + offset, size - offset);
            } while (count < 0 && errno == EINTR);
            if (count <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    void close_output() noexcept {
        if (output_ >= 0) {
            static_cast<void>(::close(output_));
            output_ = -1;
        }
    }

    pid_t pid_{-1};
    int input_{-1};
    int output_{-1};
};

bool deliver_post_to_two_nodes(
    ChildProcess& first, ChildProcess& second,
    const std::vector<Bytes>& frames,
    const Bytes& expected_canonical,
    std::vector<Bytes>& first_commits,
    std::vector<Bytes>& second_commits) {
    const auto deliver_one =
        [&expected_canonical](
            ChildProcess& child, const Bytes& frame,
            std::vector<Bytes>& commits) {
            if (!child.send_record(frame)) {
                return false;
            }
            Response response = Response::Conflict;
            Bytes canonical_post;
            if (child.read_response(
                    response, canonical_post, 1'000) !=
                ChildProcess::ReadResult::Received) {
                return false;
            }
            if (response == Response::Committed) {
                if (canonical_post != expected_canonical) {
                    return false;
                }
                commits.push_back(std::move(canonical_post));
                return true;
            }
            return response == Response::FragmentAccepted &&
                   canonical_post.empty();
        };

    for (auto frame = frames.rbegin(); frame != frames.rend();
         ++frame) {
        if (!deliver_one(first, *frame, first_commits) ||
            !deliver_one(second, *frame, second_commits)) {
            return false;
        }
    }
    return true;
}

bool replay_is_duplicate_on_two_nodes(
    ChildProcess& first, ChildProcess& second,
    const std::vector<Bytes>& frames) {
    const auto replay_one =
        [](ChildProcess& child, const Bytes& frame) {
            if (!child.send_record(frame)) {
                return false;
            }
            Response response = Response::Conflict;
            Bytes canonical_post;
            return child.read_response(
                       response, canonical_post, 1'000) ==
                       ChildProcess::ReadResult::Received &&
                   response == Response::DuplicateFrame &&
                   canonical_post.empty();
        };
    for (const auto& frame : frames) {
        if (!replay_one(first, frame) ||
            !replay_one(second, frame)) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> locate_node_helper(
    int argc, char** argv) {
    if (argc >= 2) {
        const std::filesystem::path explicit_path(argv[1]);
        if (::access(explicit_path.c_str(), X_OK) == 0) {
            return explicit_path;
        }
    }
    if (const char* environment =
            std::getenv("LORA_PHASE4_TWO_PROCESS_NODE")) {
        const std::filesystem::path environment_path(environment);
        if (::access(environment_path.c_str(), X_OK) == 0) {
            return environment_path;
        }
    }

    std::error_code error;
    auto test_executable =
        std::filesystem::absolute(argv[0], error);
    if (error) {
        return std::nullopt;
    }
    const auto directory = test_executable.parent_path();
    constexpr std::array<std::string_view, 3U> candidate_names{
        "lora_messenger_phase4_two_process_node",
        "phase4_two_process_node",
        "lora_messenger_phase4_two_process_node_test",
    };
    for (const auto name : candidate_names) {
        const auto candidate = directory / std::string(name);
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool exited_successfully(int status) noexcept {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main(int argc, char** argv) {
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    lora::test::Runner runner;

    runner.run(
        "two-node deterministic matrix covers 10000 integer seeds",
        [&] {
            std::array<std::size_t, kGateMtus.size()> mtu_coverage{};
            std::array<std::size_t, kFaultProfiles.size()>
                profile_coverage{};
            for (std::uint32_t seed = 0U;
                 seed < kStressSeedCount; ++seed) {
                const std::size_t mtu_index =
                    static_cast<std::size_t>(seed) %
                    kGateMtus.size();
                const std::size_t profile_index =
                    (static_cast<std::size_t>(seed) /
                     kGateMtus.size()) %
                    kFaultProfiles.size();
                ++mtu_coverage[mtu_index];
                ++profile_coverage[profile_index];

                const auto result = run_simulation(
                    seed, kGateMtus[mtu_index],
                    kFaultProfiles[profile_index]);
                if (!result.passed) {
                    std::cerr
                        << "seed=" << seed
                        << " mtu=" << kGateMtus[mtu_index]
                        << " fault="
                        << fault_profile_name(
                               kFaultProfiles[profile_index])
                        << " detail=" << result.detail << '\n';
                    CHECK(result.passed);
                    return;
                }
            }
            for (const auto count : mtu_coverage) {
                CHECK(count != 0U);
            }
            for (const auto count : profile_coverage) {
                CHECK(count != 0U);
            }
        });

    runner.run("inbound queue enforces its exact frame bound", [&] {
        auto post = maximum_post(99U, 0U);
        REQUIRE(post.has_value());
        auto frames = lora::protocol::fragment_post(*post, 48U);
        REQUIRE(frames.has_value());
        REQUIRE(!frames.value().empty());

        lora::protocol::InboundFrameQueue inbound(48U);
        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumInboundFrames;
             ++index) {
            CHECK_EQ(
                inbound.enqueue(
                    frames.value().front().data(),
                    frames.value().front().size(), 0U),
                IngressEnqueueStatus::Accepted);
        }
        CHECK_EQ(inbound.queued_count(),
                 lora::protocol::kMaximumInboundFrames);
        CHECK_EQ(inbound.metrics().queue_high_water,
                 lora::protocol::kMaximumInboundFrames);
        CHECK_EQ(
            inbound.enqueue(
                frames.value().front().data(),
                frames.value().front().size(), 0U),
            IngressEnqueueStatus::QueueFull);

        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumInboundFrames;
             ++index) {
            const auto processed = inbound.process_next(0U);
            CHECK(processed.status ==
                      IngressProcessStatus::FragmentAccepted ||
                  processed.status ==
                      IngressProcessStatus::DuplicateFragment);
        }
        CHECK_EQ(inbound.queued_count(), 0U);
        static_cast<void>(inbound.process_next(
            lora::protocol::kReassemblyTimeoutTicks));
        CHECK_EQ(inbound.metrics().active_reassemblies, 0U);
    });

    runner.run(
        "two simultaneous processes converge on canonical post sets",
        [&] {
            const auto helper = locate_node_helper(argc, argv);
            REQUIRE(helper.has_value());

            for (std::size_t mtu_index = 0U;
                 mtu_index < kGateMtus.size(); ++mtu_index) {
                const auto mtu = kGateMtus[mtu_index];
                auto alpha_post = maximum_post(
                    static_cast<std::uint32_t>(8'000U + mtu_index),
                    0U);
                auto bravo_post = maximum_post(
                    static_cast<std::uint32_t>(8'000U + mtu_index),
                    1U);
                REQUIRE(alpha_post.has_value());
                REQUIRE(bravo_post.has_value());
                auto alpha_encoded =
                    lora::protocol::encode_post(*alpha_post);
                auto bravo_encoded =
                    lora::protocol::encode_post(*bravo_post);
                auto alpha_frames =
                    lora::protocol::fragment_post(*alpha_post, mtu);
                auto bravo_frames =
                    lora::protocol::fragment_post(*bravo_post, mtu);
                REQUIRE(alpha_encoded.has_value());
                REQUIRE(bravo_encoded.has_value());
                REQUIRE(alpha_frames.has_value());
                REQUIRE(bravo_frames.has_value());

                ChildProcess alpha;
                ChildProcess bravo;
                REQUIRE(alpha.spawn(*helper, mtu));
                REQUIRE(bravo.spawn(*helper, mtu));
                std::vector<Bytes> alpha_commits;
                std::vector<Bytes> bravo_commits;
                REQUIRE(deliver_post_to_two_nodes(
                    alpha, bravo, alpha_frames.value(),
                    alpha_encoded.value(), alpha_commits,
                    bravo_commits));
                REQUIRE(deliver_post_to_two_nodes(
                    alpha, bravo, bravo_frames.value(),
                    bravo_encoded.value(), alpha_commits,
                    bravo_commits));
                REQUIRE(replay_is_duplicate_on_two_nodes(
                    alpha, bravo, alpha_frames.value()));
                REQUIRE(replay_is_duplicate_on_two_nodes(
                    alpha, bravo, bravo_frames.value()));

                std::vector<Bytes> expected{
                    alpha_encoded.value(), bravo_encoded.value()};
                std::sort(expected.begin(), expected.end());
                std::sort(alpha_commits.begin(),
                          alpha_commits.end());
                std::sort(bravo_commits.begin(),
                          bravo_commits.end());
                CHECK_EQ(alpha_commits, expected);
                CHECK_EQ(bravo_commits, expected);
                CHECK_EQ(alpha_commits, bravo_commits);

                Response response = Response::Conflict;
                Bytes canonical_post;
                REQUIRE(alpha.request_shutdown());
                REQUIRE(bravo.request_shutdown());
                REQUIRE(alpha.read_response(
                            response, canonical_post, 1'000) ==
                        ChildProcess::ReadResult::Received);
                CHECK_EQ(response, Response::Shutdown);
                CHECK(canonical_post.empty());
                REQUIRE(bravo.read_response(
                            response, canonical_post, 1'000) ==
                        ChildProcess::ReadResult::Received);
                CHECK_EQ(response, Response::Shutdown);
                CHECK(canonical_post.empty());
                alpha.close_input();
                bravo.close_input();
                int alpha_status = 0;
                int bravo_status = 0;
                REQUIRE(alpha.wait_for_exit(
                    2'000, alpha_status));
                REQUIRE(bravo.wait_for_exit(
                    2'000, bravo_status));
                CHECK(exited_successfully(alpha_status));
                CHECK(exited_successfully(bravo_status));
            }
        });

    runner.run(
        "fork exec pipes handle HUP timeout restart and clean shutdown",
        [&] {
            const auto helper = locate_node_helper(argc, argv);
            REQUIRE(helper.has_value());

            ChildProcess hup_child;
            REQUIRE(hup_child.spawn(*helper, 48U));
            hup_child.close_input();
            int hup_status = 0;
            REQUIRE(hup_child.wait_for_exit(2'000, hup_status));
            CHECK(exited_successfully(hup_status));

            ChildProcess timeout_child;
            REQUIRE(timeout_child.spawn(*helper, 48U));
            Response response = Response::Conflict;
            CHECK_EQ(
                timeout_child.read_response(response, 100),
                ChildProcess::ReadResult::TimedOut);
            REQUIRE(timeout_child.request_shutdown());
            REQUIRE(
                timeout_child.read_response(response, 1'000) ==
                ChildProcess::ReadResult::Received);
            CHECK_EQ(response, Response::Shutdown);
            timeout_child.close_input();
            int timeout_status = 0;
            REQUIRE(timeout_child.wait_for_exit(
                2'000, timeout_status));
            CHECK(exited_successfully(timeout_status));

            auto post = maximum_post(7'777U, 0U);
            REQUIRE(post.has_value());
            auto frames =
                lora::protocol::fragment_post(*post, 48U);
            REQUIRE(frames.has_value());
            REQUIRE(frames.value().size() > 1U);

            ChildProcess receiver;
            REQUIRE(receiver.spawn(*helper, 48U));
            std::size_t commits = 0U;
            for (auto frame = frames.value().rbegin();
                 frame != frames.value().rend(); ++frame) {
                REQUIRE(frame->size() <= 48U);
                REQUIRE(receiver.send_record(*frame));
                REQUIRE(
                    receiver.read_response(response, 1'000) ==
                    ChildProcess::ReadResult::Received);
                if (response == Response::Committed) {
                    ++commits;
                } else {
                    CHECK_EQ(response,
                             Response::FragmentAccepted);
                }
            }
            CHECK_EQ(commits, 1U);

            for (const auto& frame : frames.value()) {
                REQUIRE(receiver.send_record(frame));
                REQUIRE(
                    receiver.read_response(response, 1'000) ==
                    ChildProcess::ReadResult::Received);
                CHECK_EQ(response, Response::DuplicateFrame);
            }
            CHECK_EQ(commits, 1U);

            Bytes corrupted = frames.value().front();
            corrupted.front() ^= 0x01U;
            REQUIRE(receiver.send_record(corrupted));
            REQUIRE(
                receiver.read_response(response, 1'000) ==
                ChildProcess::ReadResult::Received);
            CHECK_EQ(response, Response::MalformedFrame);

            REQUIRE(receiver.request_shutdown());
            REQUIRE(
                receiver.read_response(response, 1'000) ==
                ChildProcess::ReadResult::Received);
            CHECK_EQ(response, Response::Shutdown);
            receiver.close_input();
            int receiver_status = 0;
            REQUIRE(receiver.wait_for_exit(
                2'000, receiver_status));
            CHECK(exited_successfully(receiver_status));
        });

    return runner.finish();
}
