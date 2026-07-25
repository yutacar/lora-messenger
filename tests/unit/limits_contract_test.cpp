/*
 * SPDX-License-Identifier: MIT
 */

#include "core/limits.h"

#include "test_support.h"

int main() {
    lora::test::Runner runner;

    runner.run("phase 1 resource limits are explicit contracts", [&] {
        CHECK_EQ(lora::core::kMaxUserIdBytes, 24U);
        CHECK_EQ(lora::core::kMaxPostBodyBytes, 160U);
        CHECK_EQ(lora::core::kMaxMentions, 4U);
        CHECK_EQ(lora::core::kMaxQueuedLocalPosts, 16U);
        CHECK_EQ(lora::core::kMaxTimelineEntries, 256U);
    });

    return runner.finish();
}
