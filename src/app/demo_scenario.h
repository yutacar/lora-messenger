/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "application/messenger_state.h"

#include <memory>
#include <string_view>

namespace app {

// A deterministic, session-only fixture for exercising the Phase 2 UI. It has no
// transport, worker, persistence, or radio side effects.
class DemoScenario {
public:
    DemoScenario();
    ~DemoScenario();

    DemoScenario(const DemoScenario&) = delete;
    DemoScenario& operator=(const DemoScenario&) = delete;

    bool ready() const noexcept;
    std::string_view error() const noexcept;
    lora::application::MessengerState& state() noexcept;
    const lora::application::MessengerState& state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace app
