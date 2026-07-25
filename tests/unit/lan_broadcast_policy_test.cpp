/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/lan_broadcast_policy.h"

#include "test_support.h"

#include <limits>

int main() {
    lora::test::Runner runner;
    using lora::adapters::network::LanBroadcastPolicy;
    using lora::adapters::network::LanBroadcastPolicyConfig;
    using lora::ports::RadioTransmitStatus;

    runner.run("invalid configurations remain locked", [&] {
        LanBroadcastPolicy invalid_gap(
            LanBroadcastPolicyConfig{0U, 4096U, 64U});
        CHECK(!invalid_gap.valid());
        CHECK_EQ(
            invalid_gap.evaluate_transmit(0U, 48U).status,
            RadioTransmitStatus::Invalid);

        LanBroadcastPolicy invalid_bucket(
            LanBroadcastPolicyConfig{10U, 254U, 64U});
        CHECK(!invalid_bucket.valid());
    });

    runner.run("minimum gap and byte budget defer deterministically", [&] {
        LanBroadcastPolicy policy(
            LanBroadcastPolicyConfig{10U, 255U, 10U});
        REQUIRE(
            policy.evaluate_transmit(100U, 200U).status ==
            RadioTransmitStatus::Allowed);
        policy.record_transmit(100U, 200U);
        CHECK_EQ(policy.available_bytes(), 55U);

        const auto gap = policy.evaluate_transmit(105U, 100U);
        CHECK_EQ(gap.status, RadioTransmitStatus::Deferred);
        CHECK_EQ(gap.not_before_tick, 110U);

        const auto budget =
            policy.evaluate_transmit(110U, 200U);
        CHECK_EQ(budget.status, RadioTransmitStatus::Deferred);
        CHECK_EQ(budget.not_before_tick, 115U);
        CHECK_EQ(
            policy.evaluate_transmit(115U, 200U).status,
            RadioTransmitStatus::Allowed);
    });

    runner.run("clock rollback locks the policy", [&] {
        LanBroadcastPolicy policy;
        REQUIRE(
            policy.evaluate_transmit(20U, 48U).status ==
            RadioTransmitStatus::Allowed);
        CHECK_EQ(
            policy.evaluate_transmit(19U, 48U).status,
            RadioTransmitStatus::Locked);
        CHECK(!policy.valid());
    });

    runner.run("saturating tick arithmetic does not wrap", [&] {
        LanBroadcastPolicy policy(
            LanBroadcastPolicyConfig{10U, 255U, 1U});
        const auto maximum =
            std::numeric_limits<lora::ports::RadioTick>::max();
        REQUIRE(
            policy.evaluate_transmit(maximum - 1U, 255U).status ==
            RadioTransmitStatus::Allowed);
        policy.record_transmit(maximum - 1U, 255U);
        const auto decision =
            policy.evaluate_transmit(maximum, 255U);
        CHECK_EQ(decision.status, RadioTransmitStatus::Deferred);
        CHECK_EQ(decision.not_before_tick, maximum);
    });

    return runner.finish();
}
