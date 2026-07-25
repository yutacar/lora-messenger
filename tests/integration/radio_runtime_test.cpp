/*
 * SPDX-License-Identifier: MIT
 */

#include "radio_runtime.h"

#include "adapters/transport/simulated_radio_bus.h"
#include "model/post.h"

#include "../unit/test_support.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const char* prefix) {
        std::array<char, 64U> pattern{};
        const std::string value =
            std::string("/tmp/") + prefix + "-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const char* created = ::mkdtemp(pattern.data());
        if (created) {
            root_ = std::filesystem::canonical(created);
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path config() const {
        return root_ / "config";
    }
    std::filesystem::path data() const {
        return root_ / "data";
    }
    bool ready() const noexcept {
        return !root_.empty();
    }

private:
    std::filesystem::path root_;
};

class EnvironmentGuard {
public:
    EnvironmentGuard()
        : config_(read("XDG_CONFIG_HOME")),
          data_(read("XDG_DATA_HOME")),
          home_(read("HOME")) {}

    ~EnvironmentGuard() {
        restore("XDG_CONFIG_HOME", config_);
        restore("XDG_DATA_HOME", data_);
        restore("HOME", home_);
    }

    void select(const TemporaryDirectory& directory) {
        static_cast<void>(::setenv(
            "XDG_CONFIG_HOME", directory.config().c_str(), 1));
        static_cast<void>(::setenv(
            "XDG_DATA_HOME", directory.data().c_str(), 1));
        static_cast<void>(::setenv("HOME", "/nonexistent", 1));
    }

private:
    static std::optional<std::string> read(const char* name) {
        const char* value = std::getenv(name);
        return value
            ? std::optional<std::string>{value}
            : std::nullopt;
    }

    static void restore(
        const char* name,
        const std::optional<std::string>& value) {
        if (value) {
            static_cast<void>(
                ::setenv(name, value->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name));
        }
    }

    std::optional<std::string> config_;
    std::optional<std::string> data_;
    std::optional<std::string> home_;
};

class AllowRadioPolicy final : public lora::ports::IRadioPolicy {
public:
    lora::ports::RadioTransmitDecision evaluate_transmit(
        lora::ports::RadioTick now,
        std::size_t) noexcept override {
        return {
            lora::ports::RadioTransmitStatus::Allowed, now};
    }

    void record_transmit(
        lora::ports::RadioTick,
        std::size_t) noexcept override {
        ++record_count;
    }

    std::size_t record_count{0U};
};

} // namespace

int main() {
    lora::test::Runner runner;

    runner.run(
        "two durable sessions broadcast receive and suppress repeat",
        [&] {
            TemporaryDirectory first_directory("lora-radio-a");
            TemporaryDirectory second_directory("lora-radio-b");
            REQUIRE(first_directory.ready());
            REQUIRE(second_directory.ready());
            EnvironmentGuard environment;

            environment.select(first_directory);
            app::PersistentSession first_session;
            REQUIRE(first_session.ready());
            environment.select(second_directory);
            app::PersistentSession second_session;
            REQUIRE(second_session.ready());

            lora::adapters::transport::SimulatedRadioBus bus({
                lora::ports::kMinimumDatagramMtu,
                16U, 16U, 16U});
            REQUIRE(bus.valid());
            AllowRadioPolicy first_policy;
            AllowRadioPolicy second_policy;
            app::RadioRuntime first_runtime(
                bus.first_endpoint(), first_policy,
                first_session.random_source());
            app::RadioRuntime second_runtime(
                bus.second_endpoint(), second_policy,
                second_session.random_source());
            REQUIRE(first_runtime.ready());
            REQUIRE(second_runtime.ready());

            auto composed = first_session.state().compose(
                lora::model::PostDraft{
                    "Phase 6 radio runtime", {},
                    std::nullopt});
            REQUIRE(composed.ok());
            REQUIRE(composed.message_id.has_value());

            for (lora::ports::RadioTick now = 0U;
                 now <= 5'000U; now += 10U) {
                REQUIRE(bus.advance_to(now));
                static_cast<void>(
                    first_runtime.pump(now, first_session));
                static_cast<void>(
                    second_runtime.pump(now, second_session));
            }

            const auto* local =
                first_session.state().timeline().find(
                    *composed.message_id);
            REQUIRE(local != nullptr);
            const auto* delivery =
                std::get_if<lora::model::LocalDelivery>(
                    &local->origin);
            REQUIRE(delivery != nullptr);
            CHECK_EQ(
                delivery->state,
                lora::model::LocalDeliveryState::Broadcast);

            CHECK_EQ(
                second_session.state().timeline().size(), 1U);
            const auto* received =
                second_session.state().timeline().newest_at(0U);
            REQUIRE(received != nullptr);
            CHECK_EQ(
                received->post.message_id(),
                *composed.message_id);
            CHECK_EQ(
                received->post.body().value(),
                "Phase 6 radio runtime");
            CHECK(std::holds_alternative<
                  lora::model::ReceivedOrigin>(
                received->origin));

            CHECK_EQ(
                first_runtime.metrics().broadcast_posts, 1U);
            CHECK_EQ(
                second_runtime.metrics().completed_posts, 1U);
            CHECK(
                second_runtime.metrics().duplicate_posts <= 1U);
            CHECK(first_policy.record_count > 0U);

            first_runtime.stop();
            second_runtime.stop();
            CHECK(!first_runtime.ready());
            CHECK(!second_runtime.ready());
            first_session.shutdown();
            second_session.shutdown();
        });

    runner.run(
        "temporary link loss preserves queued work until reconnect",
        [&] {
            TemporaryDirectory directory("lora-radio-reconnect");
            REQUIRE(directory.ready());
            EnvironmentGuard environment;
            environment.select(directory);
            app::PersistentSession session;
            REQUIRE(session.ready());

            lora::adapters::transport::SimulatedRadioBus bus({
                lora::ports::kMinimumDatagramMtu,
                16U, 16U, 16U});
            REQUIRE(bus.valid());
            AllowRadioPolicy policy;
            app::RadioRuntime runtime(
                bus.first_endpoint(), policy,
                session.random_source());
            REQUIRE(runtime.ready());
            REQUIRE(bus.set_connected(
                lora::adapters::transport::SimulatedRadioBus::
                    EndpointId::First,
                false));
            CHECK(!runtime.ready());

            auto composed = session.state().compose(
                lora::model::PostDraft{
                    "Reconnect keeps queued", {},
                    std::nullopt});
            REQUIRE(composed.ok());
            REQUIRE(composed.message_id.has_value());
            for (lora::ports::RadioTick now = 0U;
                 now <= 1'000U; now += 10U) {
                REQUIRE(bus.advance_to(now));
                CHECK(!runtime.pump(now, session));
            }
            const auto* queued =
                session.state().timeline().find(
                    *composed.message_id);
            REQUIRE(queued != nullptr);
            const auto* queued_delivery =
                std::get_if<lora::model::LocalDelivery>(
                    &queued->origin);
            REQUIRE(queued_delivery != nullptr);
            CHECK_EQ(
                queued_delivery->state,
                lora::model::LocalDeliveryState::Queued);

            REQUIRE(bus.set_connected(
                lora::adapters::transport::SimulatedRadioBus::
                    EndpointId::First,
                true));
            for (lora::ports::RadioTick now = 1'010U;
                 now <= 6'000U; now += 10U) {
                REQUIRE(bus.advance_to(now));
                static_cast<void>(runtime.pump(now, session));
            }
            const auto* broadcast =
                session.state().timeline().find(
                    *composed.message_id);
            REQUIRE(broadcast != nullptr);
            const auto* broadcast_delivery =
                std::get_if<lora::model::LocalDelivery>(
                    &broadcast->origin);
            REQUIRE(broadcast_delivery != nullptr);
            CHECK_EQ(
                broadcast_delivery->state,
                lora::model::LocalDeliveryState::Broadcast);
        });

    runner.run(
        "permanently closed transport terminates queued work",
        [&] {
            TemporaryDirectory directory("lora-radio-closed");
            REQUIRE(directory.ready());
            EnvironmentGuard environment;
            environment.select(directory);
            app::PersistentSession session;
            REQUIRE(session.ready());

            lora::adapters::transport::SimulatedRadioBus bus({
                lora::ports::kMinimumDatagramMtu,
                16U, 16U, 16U});
            REQUIRE(bus.valid());
            AllowRadioPolicy policy;
            app::RadioRuntime runtime(
                bus.first_endpoint(), policy,
                session.random_source());
            REQUIRE(runtime.ready());
            bus.first_endpoint().close();
            CHECK(!runtime.ready());

            auto composed = session.state().compose(
                lora::model::PostDraft{
                    "Closed transport fails", {},
                    std::nullopt});
            REQUIRE(composed.ok());
            REQUIRE(composed.message_id.has_value());
            for (lora::ports::RadioTick now = 0U;
                 now <= 6'000U; now += 10U) {
                REQUIRE(bus.advance_to(now));
                static_cast<void>(runtime.pump(now, session));
            }

            const auto* failed =
                session.state().timeline().find(
                    *composed.message_id);
            REQUIRE(failed != nullptr);
            const auto* failed_delivery =
                std::get_if<lora::model::LocalDelivery>(
                    &failed->origin);
            REQUIRE(failed_delivery != nullptr);
            CHECK_EQ(
                failed_delivery->state,
                lora::model::LocalDeliveryState::Failed);
            CHECK_EQ(runtime.metrics().failed_posts, 1U);
        });

    return runner.finish();
}
