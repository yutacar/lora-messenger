/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ports/clock.h"
#include "ports/random.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace lora::test {

class ScriptedRandom final : public ports::IRandomBytes {
public:
    void push_bytes(std::vector<std::uint8_t> bytes) {
        script_.push_back(std::move(bytes));
    }

    void push_seed(std::uint8_t seed, std::size_t size = 16) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t index = 0; index < size; ++index) {
            bytes[index] = static_cast<std::uint8_t>(seed + index);
        }
        push_bytes(std::move(bytes));
    }

    void fail_next() { fail_next_ = true; }

    bool fill(std::uint8_t* destination, std::size_t size) noexcept override {
        ++call_count_;
        last_requested_size_ = size;
        if (fail_next_) {
            fail_next_ = false;
            return false;
        }
        if (script_.empty() || script_.front().size() != size) {
            return false;
        }
        const auto bytes = std::move(script_.front());
        script_.pop_front();
        std::copy(bytes.begin(), bytes.end(), destination);
        return true;
    }

    std::size_t call_count() const noexcept { return call_count_; }
    std::size_t last_requested_size() const noexcept { return last_requested_size_; }
    std::size_t remaining() const noexcept { return script_.size(); }

private:
    std::deque<std::vector<std::uint8_t>> script_;
    std::size_t call_count_{0};
    std::size_t last_requested_size_{0};
    bool fail_next_{false};
};

class FakeClock final : public ports::IWallClock {
public:
    explicit FakeClock(std::optional<ports::UnixSeconds> value = std::nullopt)
        : value_(value) {}

    std::optional<ports::UnixSeconds> now_unix_seconds() noexcept override {
        ++call_count_;
        return value_;
    }

    void set(std::optional<ports::UnixSeconds> value) { value_ = value; }
    std::size_t call_count() const noexcept { return call_count_; }

private:
    std::optional<ports::UnixSeconds> value_;
    std::size_t call_count_{0};
};

} // namespace lora::test
