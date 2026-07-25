/*
 * SPDX-License-Identifier: MIT
 */

#include "model/identity.h"

#include "test_support.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace {

lora::core::UserId user_id(const char* value) {
    auto result = lora::core::UserId::create(value);
    return std::move(result).value();
}
} // namespace

int main() {
    using lora::model::Identity;
    lora::test::Runner runner;

    runner.run("a restored identity keeps its stable install ID and user ID", [&] {
        const auto install_id = lora::test::make_install_id(10);
        auto identity = Identity::restore(install_id, user_id("alice"));
        CHECK_EQ(identity.install_id(), install_id);
        CHECK_EQ(identity.user_id().value(), "alice");
        CHECK_EQ(identity.last_issued_sender_sequence(), 0U);
        REQUIRE(identity.next_sender_sequence().has_value());
        CHECK_EQ(*identity.next_sender_sequence(), 1U);
    });

    runner.run("sender sequences commit only the exact next value", [&] {
        auto identity = Identity::restore(
            lora::test::make_install_id(11), user_id("alice"), 41U);
        REQUIRE(identity.next_sender_sequence().has_value());
        CHECK_EQ(*identity.next_sender_sequence(), 42U);

        CHECK(!identity.commit_sender_sequence(43U));
        CHECK_EQ(identity.last_issued_sender_sequence(), 41U);
        CHECK(!identity.commit_sender_sequence(41U));
        CHECK_EQ(identity.last_issued_sender_sequence(), 41U);

        CHECK(identity.commit_sender_sequence(42U));
        CHECK_EQ(identity.last_issued_sender_sequence(), 42U);
        REQUIRE(identity.next_sender_sequence().has_value());
        CHECK_EQ(*identity.next_sender_sequence(), 43U);
    });

    runner.run("sequence issuance reaches UINT64_MAX once and never wraps", [&] {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        auto identity = Identity::restore(
            lora::test::make_install_id(12), user_id("alice"), maximum - 1U);
        REQUIRE(identity.next_sender_sequence().has_value());
        CHECK_EQ(*identity.next_sender_sequence(), maximum);
        CHECK(identity.commit_sender_sequence(maximum));
        CHECK_EQ(identity.last_issued_sender_sequence(), maximum);
        CHECK(!identity.next_sender_sequence().has_value());
        CHECK(!identity.commit_sender_sequence(0U));
        CHECK(!identity.commit_sender_sequence(1U));
        CHECK_EQ(identity.last_issued_sender_sequence(), maximum);
    });

    runner.run("rename changes only future user identity snapshots", [&] {
        const auto install_id = lora::test::make_install_id(13);
        auto identity = Identity::restore(install_id, user_id("alice"), 7U);
        identity.rename(user_id("bob"));
        CHECK_EQ(identity.install_id(), install_id);
        CHECK_EQ(identity.user_id().value(), "bob");
        CHECK_EQ(identity.last_issued_sender_sequence(), 7U);
        REQUIRE(identity.next_sender_sequence().has_value());
        CHECK_EQ(*identity.next_sender_sequence(), 8U);
    });

    return runner.finish();
}
