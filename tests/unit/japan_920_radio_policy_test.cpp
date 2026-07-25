/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/radio/japan_920_radio_policy.h"

#include "ports/datagram_transport.h"
#include "test_support.h"

#include <cstddef>

int main() {
    lora::test::Runner runner;
    using lora::adapters::radio::Japan920PolicyConfig;
    using lora::adapters::radio::Japan920Profile;
    using lora::adapters::radio::Japan920RadioPolicy;
    using lora::ports::RadioTransmitStatus;

    runner.run("approved default profile is bounded to JP Cap range", [&] {
        Japan920Profile profile;
        CHECK(profile.valid());

        profile.transmit_power_dbm = 14U;
        CHECK(!profile.valid());
        profile = {};
        profile.frequency_hz = 920'500'000U;
        CHECK(!profile.valid());
        profile = {};
        profile.frequency_hz = 923'000'000U;
        CHECK(!profile.valid());
        profile = {};
        profile.bandwidth_hz = 250'000U;
        CHECK(!profile.valid());
        profile = {};
        profile.sync_word = 0x34U;
        CHECK(!profile.valid());
    });

    runner.run("airtime estimate grows with payload and slow profile", [&] {
        Japan920RadioPolicy policy;
        REQUIRE(policy.valid());
        const auto one = policy.estimated_airtime_ms(1U);
        const auto minimum =
            policy.estimated_airtime_ms(
                lora::ports::kMinimumDatagramMtu);
        const auto maximum =
            policy.estimated_airtime_ms(
                lora::ports::kMaximumDatagramBytes);
        CHECK(one > 0U);
        CHECK(minimum > one);
        CHECK(maximum > minimum);
        CHECK_EQ(policy.estimated_airtime_ms(0U), 0U);
        CHECK_EQ(
            policy.estimated_airtime_ms(
                lora::ports::kMaximumDatagramBytes + 1U),
            0U);

        Japan920PolicyConfig slow;
        slow.profile.spreading_factor = 12U;
        Japan920RadioPolicy slow_policy(slow);
        REQUIRE(slow_policy.valid());
        CHECK(
            slow_policy.estimated_airtime_ms(48U) >
            minimum);
    });

    runner.run("minimum gap defers and then permits next datagram", [&] {
        Japan920RadioPolicy policy;
        REQUIRE(
            policy.evaluate_transmit(1'000U, 48U).status ==
            RadioTransmitStatus::Allowed);
        policy.record_transmit(1'000U, 48U);
        REQUIRE(policy.valid());
        const auto early = policy.evaluate_transmit(1'099U, 48U);
        CHECK_EQ(early.status, RadioTransmitStatus::Deferred);
        CHECK_EQ(early.not_before_tick, 1'100U);
        CHECK_EQ(
            policy.evaluate_transmit(1'100U, 48U).status,
            RadioTransmitStatus::Allowed);
    });

    runner.run("token bucket bounds congestion and refills", [&] {
        Japan920PolicyConfig config;
        Japan920RadioPolicy probe(config);
        const auto airtime = probe.estimated_airtime_ms(255U);
        REQUIRE(airtime > 0U);
        config.bucket_capacity_ms = airtime * 2U;
        config.minimum_gap_ms = 1U;
        Japan920RadioPolicy policy(config);
        REQUIRE(policy.valid());

        REQUIRE(
            policy.evaluate_transmit(0U, 255U).status ==
            RadioTransmitStatus::Allowed);
        policy.record_transmit(0U, 255U);
        REQUIRE(
            policy.evaluate_transmit(1U, 255U).status ==
            RadioTransmitStatus::Allowed);
        policy.record_transmit(1U, 255U);

        const auto deferred =
            policy.evaluate_transmit(2U, 255U);
        CHECK_EQ(deferred.status, RadioTransmitStatus::Deferred);
        CHECK(deferred.not_before_tick > 2U);
        CHECK_EQ(
            policy.evaluate_transmit(
                deferred.not_before_tick, 255U).status,
            RadioTransmitStatus::Allowed);
    });

    runner.run("invalid calls and clock rollback lock safely", [&] {
        Japan920PolicyConfig invalid_config;
        invalid_config.refill_denominator = 0U;
        Japan920RadioPolicy invalid(invalid_config);
        CHECK(!invalid.valid());
        CHECK_EQ(
            invalid.evaluate_transmit(0U, 48U).status,
            RadioTransmitStatus::Invalid);

        Japan920RadioPolicy policy;
        REQUIRE(
            policy.evaluate_transmit(100U, 48U).status ==
            RadioTransmitStatus::Allowed);
        CHECK_EQ(
            policy.evaluate_transmit(99U, 48U).status,
            RadioTransmitStatus::Locked);
        CHECK(!policy.valid());
    });

    runner.run("record without an allowed decision locks the policy", [&] {
        Japan920RadioPolicy policy;
        policy.record_transmit(100U, 48U);
        REQUIRE(policy.valid());
        policy.record_transmit(100U, 48U);
        CHECK(!policy.valid());
        CHECK_EQ(
            policy.evaluate_transmit(200U, 48U).status,
            RadioTransmitStatus::Invalid);
    });

    return runner.finish();
}
