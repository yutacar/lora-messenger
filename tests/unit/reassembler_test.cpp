/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol/reassembler.h"

#include "protocol/fragmenter.h"
#include "protocol/frame_codec.h"
#include "test_model_helpers.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

lora::model::PostPayload maximum_post(
    std::uint8_t message_discriminator = 20U,
    std::uint8_t sender_discriminator = 40U,
    std::uint64_t sequence = 1U) {
    auto input = lora::test::make_post_input(
        message_discriminator, sender_discriminator, sequence,
        std::string(160U, 'b'), std::string(24U, 'u'));
    input.mentions = {
        lora::test::make_uuid(100U),
        lora::test::make_uuid(110U),
        lora::test::make_uuid(120U),
        lora::test::make_uuid(130U),
    };
    input.reply_to = lora::test::make_uuid(19U);
    input.sender_time = std::numeric_limits<std::int64_t>::max();
    auto result =
        lora::model::PostPayload::create(std::move(input));
    return std::move(result).value();
}

std::vector<lora::protocol::Bytes> encoded_frames(
    const lora::model::PostPayload& post, std::size_t mtu) {
    auto result = lora::protocol::fragment_post(post, mtu);
    return std::move(result).value();
}

std::vector<lora::protocol::DataFrame> decoded_frames(
    const lora::model::PostPayload& post, std::size_t mtu) {
    const auto encoded = encoded_frames(post, mtu);
    std::vector<lora::protocol::DataFrame> frames;
    frames.reserve(encoded.size());
    for (const auto& datagram : encoded) {
        auto result = lora::protocol::decode_frame(datagram, mtu);
        frames.push_back(std::move(result).value());
    }
    return frames;
}

} // namespace

int main() {
    using lora::protocol::IngressEnqueueStatus;
    using lora::protocol::IngressProcessStatus;
    using lora::protocol::ReassemblyStatus;
    lora::test::Runner runner;

    runner.run("out-of-order fragments converge at every required MTU", [&] {
        constexpr std::array<std::size_t, 5U> mtus{
            48U, 51U, 64U, 128U, 255U};
        const auto post = maximum_post();
        const auto logical = lora::protocol::encode_post(post);
        REQUIRE(logical.has_value());

        for (const auto mtu : mtus) {
            const auto encoded = encoded_frames(post, mtu);
            const auto frames = decoded_frames(post, mtu);
            lora::protocol::Reassembler reassembler(mtu);
            lora::protocol::ReassemblyResult result;

            for (std::size_t reverse = frames.size();
                 reverse > 0U; --reverse) {
                result = reassembler.accept(
                    frames[reverse - 1U],
                    static_cast<std::uint64_t>(
                        frames.size() - reverse));
                if (reverse > 1U) {
                    CHECK_EQ(result.status,
                             ReassemblyStatus::FragmentAccepted);
                    CHECK(!result.completed.has_value());
                }
            }

            REQUIRE(result.status == ReassemblyStatus::Complete);
            REQUIRE(result.completed.has_value());
            CHECK_EQ(result.completed->post, post);
            CHECK_EQ(result.completed->canonical_post,
                     logical.value());
            CHECK_EQ(result.completed->canonical_frames, encoded);
            CHECK_EQ(reassembler.active_count(), 0U);
            CHECK_EQ(reassembler.high_water_count(), 1U);
        }
    });

    runner.run("identical duplicates do not extend the fixed timeout", [&] {
        constexpr std::size_t mtu = 48U;
        const auto post =
            lora::test::make_post(31U, 51U, 1U, "duplicate");
        const auto frames = decoded_frames(post, mtu);
        REQUIRE(frames.size() > 1U);
        lora::protocol::Reassembler reassembler(mtu);

        CHECK_EQ(reassembler.accept(frames[0], 10U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.accept(frames[0], 59'999U).status,
                 ReassemblyStatus::DuplicateFragment);
        CHECK_EQ(reassembler.active_count(), 1U);
        CHECK_EQ(reassembler.expire(60'009U), 0U);
        CHECK_EQ(reassembler.active_count(), 1U);
        CHECK_EQ(reassembler.expire(60'010U), 1U);
        CHECK_EQ(reassembler.active_count(), 0U);
    });

    runner.run("conflicting duplicate quarantines the key until deadline",
               [&] {
        constexpr std::size_t mtu = 48U;
        const auto post =
            lora::test::make_post(32U, 52U, 2U, "conflict");
        const auto frames = decoded_frames(post, mtu);
        REQUIRE(frames.size() > 1U);
        auto altered = frames[0];
        altered.payload.back() ^= 0x01U;
        lora::protocol::Reassembler reassembler(mtu);

        CHECK_EQ(reassembler.accept(frames[0], 100U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.accept(altered, 101U).status,
                 ReassemblyStatus::Conflict);
        CHECK_EQ(reassembler.active_count(), 0U);
        CHECK_EQ(reassembler.accept(frames[0], 60'099U).status,
                 ReassemblyStatus::Quarantined);
        CHECK_EQ(reassembler.accept(frames[0], 60'100U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.active_count(), 1U);
    });

    runner.run("metadata conflicts also quarantine one logical key", [&] {
        constexpr std::size_t mtu = 48U;
        const auto short_post =
            lora::test::make_post(33U, 53U, 3U, "a");
        const auto long_post =
            lora::test::make_post(
                33U, 53U, 3U, std::string(100U, 'x'));
        const auto short_frames =
            decoded_frames(short_post, mtu);
        const auto long_frames =
            decoded_frames(long_post, mtu);
        REQUIRE(short_frames[0].fragment_count !=
                long_frames[0].fragment_count);
        lora::protocol::Reassembler reassembler(mtu);

        CHECK_EQ(reassembler.accept(short_frames[0], 0U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.accept(long_frames[0], 1U).status,
                 ReassemblyStatus::Conflict);
        CHECK_EQ(reassembler.accept(short_frames[0], 2U).status,
                 ReassemblyStatus::Quarantined);
    });

    runner.run("invalid logical bytes fail only after bounded assembly", [&] {
        constexpr std::size_t mtu = 64U;
        const auto post =
            lora::test::make_post(34U, 54U, 4U, "invalid logical post");
        auto frames = decoded_frames(post, mtu);
        REQUIRE(frames.size() > 1U);
        frames[0].payload[0] |= 0x80U;
        lora::protocol::Reassembler reassembler(mtu);

        for (std::size_t index = 0U;
             index + 1U < frames.size(); ++index) {
            CHECK_EQ(reassembler.accept(frames[index], 0U).status,
                     ReassemblyStatus::FragmentAccepted);
        }
        const auto result =
            reassembler.accept(frames.back(), 0U);
        CHECK_EQ(result.status, ReassemblyStatus::InvalidPost);
        CHECK(!result.completed.has_value());
        CHECK_EQ(reassembler.active_count(), 0U);
        CHECK_EQ(reassembler.accept(frames[0], 1U).status,
                 ReassemblyStatus::Quarantined);
    });

    runner.run("structural frame violations never allocate a slot", [&] {
        constexpr std::size_t mtu = 64U;
        const auto post =
            lora::test::make_post(35U, 55U, 5U, "shape");
        const auto frames = decoded_frames(post, mtu);
        REQUIRE(!frames.empty());
        const auto valid = frames[0];
        lora::protocol::Reassembler reassembler(mtu);

        auto invalid = valid;
        invalid.sender_sequence = 0U;
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.fragment_count = 0U;
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.fragment_count =
            static_cast<std::uint8_t>(
                lora::protocol::kMaximumFragments + 1U);
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.fragment_index = invalid.fragment_count;
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.total_length = static_cast<std::uint16_t>(
            lora::protocol::kMinimumEncodedPostBytes - 1U);
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.total_length = static_cast<std::uint16_t>(
            lora::protocol::kMaximumEncodedPostBytes + 1U);
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        invalid = valid;
        invalid.payload.pop_back();
        CHECK_EQ(reassembler.accept(invalid, 0U).status,
                 ReassemblyStatus::InvalidPost);

        lora::protocol::Reassembler invalid_mtu(47U);
        CHECK_EQ(invalid_mtu.accept(valid, 0U).status,
                 ReassemblyStatus::InvalidPost);
        CHECK_EQ(reassembler.active_count(), 0U);
        CHECK_EQ(reassembler.high_water_count(), 0U);
    });

    runner.run("active reassemblies are capped and reusable after expiry",
               [&] {
        constexpr std::size_t mtu = 48U;
        lora::protocol::Reassembler reassembler(mtu);
        std::vector<lora::protocol::DataFrame> first_frames;
        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumReassemblies + 1U;
             ++index) {
            const auto post = lora::test::make_post(
                static_cast<std::uint8_t>(70U + index), 90U,
                static_cast<std::uint64_t>(index + 1U),
                std::string(80U, 'a'));
            first_frames.push_back(
                decoded_frames(post, mtu).front());
        }

        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumReassemblies;
             ++index) {
            CHECK_EQ(reassembler.accept(
                         first_frames[index], 5U)
                         .status,
                     ReassemblyStatus::FragmentAccepted);
        }
        CHECK_EQ(reassembler.active_count(),
                 lora::protocol::kMaximumReassemblies);
        CHECK_EQ(reassembler.high_water_count(),
                 lora::protocol::kMaximumReassemblies);
        CHECK_EQ(reassembler.accept(first_frames.back(), 5U).status,
                 ReassemblyStatus::CapacityExceeded);
        CHECK_EQ(reassembler.expire(60'004U), 0U);
        CHECK_EQ(reassembler.expire(60'005U),
                 lora::protocol::kMaximumReassemblies);
        CHECK_EQ(reassembler.accept(first_frames.back(), 60'005U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.active_count(), 1U);

        reassembler.clear();
        CHECK_EQ(reassembler.active_count(), 0U);
        CHECK_EQ(reassembler.high_water_count(),
                 lora::protocol::kMaximumReassemblies);
    });

    runner.run("quarantine storage remains bounded under conflicts", [&] {
        constexpr std::size_t mtu = 48U;
        lora::protocol::Reassembler reassembler(mtu);
        std::vector<lora::protocol::DataFrame> originals;
        originals.reserve(
            lora::protocol::kMaximumQuarantinedKeys + 1U);

        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumQuarantinedKeys + 1U;
             ++index) {
            const auto discriminator =
                static_cast<std::uint8_t>(100U + index);
            const auto sequence =
                static_cast<std::uint64_t>(index + 1U);
            const auto original = lora::test::make_post(
                discriminator, 150U, sequence, "a");
            const auto conflicting = lora::test::make_post(
                discriminator, 150U, sequence,
                std::string(80U, 'z'));
            const auto original_frame =
                decoded_frames(original, mtu).front();
            const auto conflict_frame =
                decoded_frames(conflicting, mtu).front();
            originals.push_back(original_frame);
            REQUIRE(reassembler.accept(original_frame, 0U).status ==
                    ReassemblyStatus::FragmentAccepted);
            REQUIRE(reassembler.accept(conflict_frame, 0U).status ==
                    ReassemblyStatus::Conflict);
        }

        CHECK_EQ(reassembler.accept(originals.front(), 1U).status,
                 ReassemblyStatus::FragmentAccepted);
        CHECK_EQ(reassembler.accept(originals.back(), 1U).status,
                 ReassemblyStatus::Quarantined);
    });

    runner.run("ingress queue validates and caps copied datagrams", [&] {
        constexpr std::size_t mtu = 64U;
        const auto datagram = encoded_frames(
            lora::test::make_post(140U, 160U, 1U, "queue"),
            mtu)[0];
        lora::protocol::InboundFrameQueue queue(mtu);

        CHECK_EQ(queue.enqueue(nullptr, datagram.size(), 0U),
                 IngressEnqueueStatus::Invalid);
        CHECK_EQ(queue.enqueue(datagram.data(),
                               lora::protocol::kFrameHeaderBytes - 1U,
                               0U),
                 IngressEnqueueStatus::Invalid);
        std::vector<std::uint8_t> oversized(mtu + 1U, 0U);
        CHECK_EQ(queue.enqueue(oversized.data(), oversized.size(), 0U),
                 IngressEnqueueStatus::Invalid);

        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumInboundFrames; ++index) {
            REQUIRE(queue.enqueue(datagram.data(), datagram.size(),
                                  static_cast<std::uint64_t>(index)) ==
                    IngressEnqueueStatus::Accepted);
        }
        CHECK_EQ(queue.queued_count(),
                 lora::protocol::kMaximumInboundFrames);
        CHECK_EQ(queue.metrics().queued_frames,
                 lora::protocol::kMaximumInboundFrames);
        CHECK_EQ(queue.metrics().queue_high_water,
                 lora::protocol::kMaximumInboundFrames);
        CHECK_EQ(queue.enqueue(datagram.data(), datagram.size(), 0U),
                 IngressEnqueueStatus::QueueFull);
    });

    runner.run("malformed datagrams are isolated from reassembly", [&] {
        constexpr std::size_t mtu = 64U;
        auto datagram = encoded_frames(
            lora::test::make_post(141U, 161U, 2U, "checksum"),
            mtu)[0];
        datagram.back() ^= 0x80U;
        lora::protocol::InboundFrameQueue queue(mtu);

        REQUIRE(queue.enqueue(datagram.data(), datagram.size(), 0U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(0U).status,
                 IngressProcessStatus::Malformed);
        CHECK_EQ(queue.metrics().malformed_frames, 1U);
        CHECK_EQ(queue.metrics().active_reassemblies, 0U);
        CHECK_EQ(queue.process_next(0U).status,
                 IngressProcessStatus::Empty);
    });

    runner.run("ingress age rejects future and exact-timeout frames", [&] {
        constexpr std::size_t mtu = 48U;
        const auto frames = encoded_frames(
            lora::test::make_post(
                145U, 165U, 6U, std::string(80U, 'a')),
            mtu);
        REQUIRE(frames.size() > 1U);

        lora::protocol::InboundFrameQueue future(mtu);
        REQUIRE(future.enqueue(
                    frames[0].data(), frames[0].size(), 11U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(future.process_next(10U).status,
                 IngressProcessStatus::Stale);
        CHECK_EQ(future.metrics().stale_frames, 1U);
        CHECK_EQ(future.metrics().active_reassemblies, 0U);

        lora::protocol::InboundFrameQueue just_inside(mtu);
        REQUIRE(just_inside.enqueue(
                    frames[0].data(), frames[0].size(), 100U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(just_inside.process_next(60'099U).status,
                 IngressProcessStatus::FragmentAccepted);
        CHECK_EQ(just_inside.metrics().stale_frames, 0U);
        CHECK_EQ(just_inside.metrics().active_reassemblies, 1U);

        lora::protocol::InboundFrameQueue exact_timeout(mtu);
        REQUIRE(exact_timeout.enqueue(
                    frames[0].data(), frames[0].size(), 100U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(exact_timeout.process_next(60'100U).status,
                 IngressProcessStatus::Stale);
        CHECK_EQ(exact_timeout.metrics().stale_frames, 1U);
        CHECK_EQ(exact_timeout.metrics().active_reassemblies, 0U);

        constexpr auto maximum =
            std::numeric_limits<std::uint64_t>::max();
        lora::protocol::InboundFrameQueue near_tick_limit(mtu);
        REQUIRE(near_tick_limit.enqueue(
                    frames[0].data(), frames[0].size(),
                    maximum - 10U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(near_tick_limit.process_next(maximum).status,
                 IngressProcessStatus::FragmentAccepted);
        CHECK_EQ(near_tick_limit.metrics().stale_frames, 0U);
    });

    runner.run("durable commit suppresses exact frames without extending TTL",
               [&] {
        constexpr std::size_t mtu = 128U;
        const auto post =
            lora::test::make_post(142U, 162U, 3U, "committed");
        const auto frames = encoded_frames(post, mtu);
        REQUIRE(frames.size() == 1U);
        lora::protocol::InboundFrameQueue queue(mtu);

        REQUIRE(queue.enqueue(
                    frames[0].data(), frames[0].size(), 0U) ==
                IngressEnqueueStatus::Accepted);
        auto completed = queue.process_next(0U);
        REQUIRE(completed.status == IngressProcessStatus::Complete);
        REQUIRE(completed.completed.has_value());
        queue.mark_committed(
            completed.completed->canonical_frames, 100U);

        REQUIRE(queue.enqueue(
                    frames[0].data(), frames[0].size(), 50'000U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(50'000U).status,
                 IngressProcessStatus::RecentDuplicate);
        queue.mark_committed(frames, 50'000U);

        REQUIRE(queue.enqueue(
                    frames[0].data(), frames[0].size(), 60'099U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(60'099U).status,
                 IngressProcessStatus::RecentDuplicate);
        REQUIRE(queue.enqueue(
                    frames[0].data(), frames[0].size(), 60'100U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(60'100U).status,
                 IngressProcessStatus::Complete);
        CHECK_EQ(queue.metrics().duplicate_frames, 2U);
        CHECK_EQ(queue.metrics().completed_posts, 2U);
    });

    runner.run("recent frame cache evicts at its fixed bound", [&] {
        constexpr std::size_t mtu = 64U;
        lora::protocol::InboundFrameQueue queue(mtu);
        std::vector<lora::protocol::Bytes> markers;
        markers.reserve(lora::protocol::kMaximumRecentFrames + 1U);
        for (std::size_t index = 0U;
             index < lora::protocol::kMaximumRecentFrames + 1U;
             ++index) {
            lora::protocol::Bytes marker(
                lora::protocol::kFrameHeaderBytes, 0U);
            marker[0] = static_cast<std::uint8_t>(index);
            marker[1] = static_cast<std::uint8_t>(index >> 8U);
            markers.push_back(marker);
            queue.mark_committed({marker}, 0U);
        }

        REQUIRE(queue.enqueue(markers.front().data(),
                              markers.front().size(), 1U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(1U).status,
                 IngressProcessStatus::Malformed);
        REQUIRE(queue.enqueue(markers.back().data(),
                              markers.back().size(), 1U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(1U).status,
                 IngressProcessStatus::RecentDuplicate);
    });

    runner.run("ingress expiration metrics use receive time", [&] {
        constexpr std::size_t mtu = 48U;
        const auto frames = encoded_frames(
            lora::test::make_post(
                143U, 163U, 4U, std::string(80U, 't')),
            mtu);
        REQUIRE(frames.size() > 1U);
        lora::protocol::InboundFrameQueue queue(mtu);

        REQUIRE(queue.enqueue(frames[0].data(), frames[0].size(), 7U) ==
                IngressEnqueueStatus::Accepted);
        CHECK_EQ(queue.process_next(7U).status,
                 IngressProcessStatus::FragmentAccepted);
        CHECK_EQ(queue.metrics().active_reassemblies, 1U);
        CHECK_EQ(queue.process_next(60'006U).status,
                 IngressProcessStatus::Empty);
        CHECK_EQ(queue.metrics().expired_reassemblies, 0U);
        CHECK_EQ(queue.process_next(60'007U).status,
                 IngressProcessStatus::Empty);
        CHECK_EQ(queue.metrics().expired_reassemblies, 1U);
        CHECK_EQ(queue.metrics().active_reassemblies, 0U);
    });

    runner.run("stop closes and clears all ingress state", [&] {
        constexpr std::size_t mtu = 48U;
        const auto frame = encoded_frames(
            lora::test::make_post(
                144U, 164U, 5U, std::string(80U, 's')),
            mtu)[0];
        lora::protocol::InboundFrameQueue queue(mtu);
        REQUIRE(queue.enqueue(frame.data(), frame.size(), 0U) ==
                IngressEnqueueStatus::Accepted);
        queue.mark_committed({frame}, 0U);

        queue.stop();
        CHECK(queue.stopped());
        CHECK_EQ(queue.queued_count(), 0U);
        CHECK_EQ(queue.metrics().queued_frames, 0U);
        CHECK_EQ(queue.metrics().active_reassemblies, 0U);
        CHECK_EQ(queue.enqueue(frame.data(), frame.size(), 0U),
                 IngressEnqueueStatus::Closed);
        CHECK_EQ(queue.process_next(0U).status,
                 IngressProcessStatus::Closed);
    });

    return runner.finish();
}
