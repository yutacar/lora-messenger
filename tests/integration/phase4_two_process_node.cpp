/*
 * SPDX-License-Identifier: MIT
 */

#include "phase4_pipe_protocol.h"

#include "protocol/dedupe_window.h"
#include "protocol/limits.h"
#include "protocol/reassembler.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <unistd.h>

namespace {

enum class ReadStatus {
    Complete,
    EndOfFile,
    Truncated,
    Failed,
};

ReadStatus read_exact(int descriptor, std::uint8_t* destination,
                      std::size_t size) noexcept {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto count = ::read(
            descriptor, destination + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            return offset == 0U ? ReadStatus::EndOfFile
                                : ReadStatus::Truncated;
        }
        if (errno != EINTR) {
            return ReadStatus::Failed;
        }
    }
    return ReadStatus::Complete;
}

bool write_exact(int descriptor, const std::uint8_t* source,
                 std::size_t size) noexcept {
    std::size_t offset = 0U;
    while (offset < size) {
        const auto count =
            ::write(descriptor, source + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

struct DatagramResult {
    lora::test::phase4_pipe::Response response;
    lora::protocol::Bytes canonical_post;
};

bool write_response(const DatagramResult& result) noexcept {
    if (result.canonical_post.size() >
        std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const auto size =
        static_cast<std::uint16_t>(result.canonical_post.size());
    const std::array<std::uint8_t,
                     lora::test::phase4_pipe::kResponseHeaderBytes>
        header{
            static_cast<std::uint8_t>(result.response),
            static_cast<std::uint8_t>(size >> 8U),
            static_cast<std::uint8_t>(size),
        };
    return write_exact(STDOUT_FILENO, header.data(), header.size()) &&
           write_exact(STDOUT_FILENO, result.canonical_post.data(),
                       result.canonical_post.size());
}

bool parse_mtu(std::string_view value, std::size_t& mtu) noexcept {
    std::size_t parsed = 0U;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        !lora::protocol::is_supported_mtu(parsed)) {
        return false;
    }
    mtu = parsed;
    return true;
}

DatagramResult process_datagram(
    lora::protocol::InboundFrameQueue& inbound,
    lora::protocol::MessageDedupeWindow& dedupe,
    const std::uint8_t* datagram, std::size_t size,
    std::uint64_t tick) {
    using lora::protocol::DedupeClassification;
    using lora::protocol::DedupeError;
    using lora::protocol::IngressEnqueueStatus;
    using lora::protocol::IngressProcessStatus;
    using lora::test::phase4_pipe::Response;

    if (inbound.enqueue(datagram, size, tick) !=
        IngressEnqueueStatus::Accepted) {
        return {Response::QueueRejected, {}};
    }
    auto result = inbound.process_next(tick);
    switch (result.status) {
        case IngressProcessStatus::FragmentAccepted:
            return {Response::FragmentAccepted, {}};
        case IngressProcessStatus::DuplicateFragment:
        case IngressProcessStatus::RecentDuplicate:
            return {Response::DuplicateFrame, {}};
        case IngressProcessStatus::Malformed:
        case IngressProcessStatus::InvalidPost:
        case IngressProcessStatus::Stale:
            return {Response::MalformedFrame, {}};
        case IngressProcessStatus::Conflict:
        case IngressProcessStatus::Quarantined:
        case IngressProcessStatus::CapacityExceeded:
            return {Response::Conflict, {}};
        case IngressProcessStatus::Empty:
        case IngressProcessStatus::Closed:
            return {Response::QueueRejected, {}};
        case IngressProcessStatus::Complete:
            break;
    }

    if (!result.completed) {
        return {Response::QueueRejected, {}};
    }
    const auto& completed = *result.completed;
    switch (dedupe.classify(completed.post.message_id(),
                            completed.canonical_post)) {
        case DedupeClassification::New:
            if (dedupe.remember(completed.post,
                                completed.canonical_post) !=
                DedupeError::None) {
                return {Response::Conflict, {}};
            }
            inbound.mark_committed(completed.canonical_frames, tick);
            return {Response::Committed, completed.canonical_post};
        case DedupeClassification::Duplicate:
            inbound.mark_committed(completed.canonical_frames, tick);
            return {Response::DuplicatePost, completed.canonical_post};
        case DedupeClassification::Conflict:
        case DedupeClassification::Invalid:
            return {Response::Conflict, {}};
    }
    return {Response::Conflict, {}};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--mtu") {
        return 64;
    }
    std::size_t mtu = 0U;
    if (!parse_mtu(argv[2], mtu)) {
        return 64;
    }

    lora::protocol::InboundFrameQueue inbound(mtu);
    lora::protocol::MessageDedupeWindow dedupe(64U);
    std::array<std::uint8_t,
               lora::test::phase4_pipe::kMaximumRecordBytes>
        datagram{};
    std::uint64_t tick = 1U;

    for (;;) {
        std::array<std::uint8_t,
                   lora::test::phase4_pipe::kLengthPrefixBytes>
            prefix{};
        const auto prefix_status =
            read_exact(STDIN_FILENO, prefix.data(), prefix.size());
        if (prefix_status == ReadStatus::EndOfFile) {
            return 0;
        }
        if (prefix_status != ReadStatus::Complete) {
            return 65;
        }

        const std::size_t size =
            (static_cast<std::size_t>(prefix[0]) << 8U) |
            static_cast<std::size_t>(prefix[1]);
        if (size == 0U) {
            return write_response(
                       {lora::test::phase4_pipe::Response::Shutdown,
                        {}})
                       ? 0
                       : 66;
        }
        if (size > mtu || size > datagram.size()) {
            return 65;
        }
        if (read_exact(STDIN_FILENO, datagram.data(), size) !=
            ReadStatus::Complete) {
            return 65;
        }

        const auto response =
            process_datagram(inbound, dedupe, datagram.data(), size,
                             tick);
        if (!write_response(response)) {
            return 66;
        }
        if (tick == std::numeric_limits<std::uint64_t>::max()) {
            return 67;
        }
        ++tick;
    }
}
