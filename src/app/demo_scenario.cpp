/*
 * SPDX-License-Identifier: MIT
 */

#include "demo_scenario.h"

#include "core/uuid.h"
#include "model/post.h"
#include "ports/clock.h"
#include "ports/random.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {
namespace {

class DemoRandom final : public lora::ports::IRandomBytes {
public:
    bool fill(std::uint8_t* destination, std::size_t size) noexcept override {
        if (!destination || size != lora::core::Uuid::Bytes{}.size()) {
            return false;
        }
        ++request_;
        for (std::size_t index = 0; index < size; ++index) {
            destination[index] = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(request_ * 29U) +
                static_cast<std::uint8_t>(index));
        }
        return true;
    }

private:
    std::uint8_t request_{0};
};

class DemoClock final : public lora::ports::IWallClock {
public:
    std::optional<lora::ports::UnixSeconds> now_unix_seconds() noexcept override {
        return std::nullopt;
    }
};

lora::core::Uuid demo_uuid(std::uint8_t seed) {
    lora::core::Uuid::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed +
                                                 static_cast<std::uint8_t>(index));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return lora::core::Uuid::from_bytes(bytes);
}

lora::model::PostPayloadInput post_input(std::uint8_t message_seed,
                                         std::uint8_t sender_seed,
                                         std::uint64_t sender_sequence,
                                         std::string sender,
                                         std::string body) {
    lora::model::PostPayloadInput input;
    input.message_id = demo_uuid(message_seed);
    input.sender_id = demo_uuid(sender_seed);
    input.sender_sequence = sender_sequence;
    input.sender_user_id = std::move(sender);
    input.body = std::move(body);
    return input;
}

} // namespace

struct DemoScenario::Impl {
    DemoRandom random;
    DemoClock clock;
    lora::application::MessengerState messenger{random, clock};
    bool initialized{false};
    std::string failure;

    Impl() {
        const auto identity = messenger.initialize_new("Mika");
        if (!identity.ok()) {
            failure = "failed to initialize the session identity";
            return;
        }

        auto first_input = post_input(0x41U, 0x81U, 1, "Sora",
                                      "Sunset meetup at the west gate.");
        auto first = lora::model::PostPayload::create(std::move(first_input));
        if (!first) {
            failure = "failed to create the first demo post";
            return;
        }
        const auto first_id = first.value().message_id();
        if (!messenger.accept_received(std::move(first).value()).ok()) {
            failure = "failed to insert the first demo post";
            return;
        }

        auto second_input = post_input(0x51U, 0x91U, 1, "Lin",
                                       "Local demo: radio remains disabled.");
        second_input.mentions.push_back(
            messenger.identity()->install_id().uuid());
        auto second = lora::model::PostPayload::create(std::move(second_input));
        if (!second || !messenger.accept_received(std::move(second).value()).ok()) {
            failure = "failed to insert the second demo post";
            return;
        }

        auto reply_input = post_input(0x61U, 0x81U, 2, "Sora",
                                      "Signal check 🔒 stays local. This longer test post verifies "
                                      "keyboard scrolling; there is no radio or delivery "
                                      "confirmation.");
        reply_input.reply_to = first_id.uuid();
        auto reply = lora::model::PostPayload::create(std::move(reply_input));
        if (!reply || !messenger.accept_received(std::move(reply).value()).ok()) {
            failure = "failed to insert the reply demo post";
            return;
        }

        initialized = true;
    }
};

DemoScenario::DemoScenario() : impl_(std::make_unique<Impl>()) {}

DemoScenario::~DemoScenario() = default;

bool DemoScenario::ready() const noexcept {
    return impl_ && impl_->initialized;
}

std::string_view DemoScenario::error() const noexcept {
    return impl_ ? std::string_view{impl_->failure} : std::string_view{};
}

lora::application::MessengerState& DemoScenario::state() noexcept {
    return impl_->messenger;
}

const lora::application::MessengerState& DemoScenario::state() const noexcept {
    return impl_->messenger;
}

} // namespace app
