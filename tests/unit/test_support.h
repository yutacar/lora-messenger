/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "core/uuid.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace lora::test {

class Runner {
public:
    template<typename Test>
    void run(std::string_view name, Test&& test) {
        ++case_count_;
        current_case_ = std::string(name);
        const auto failures_before = failure_count_;
        try {
            std::forward<Test>(test)();
        } catch (const std::exception& error) {
            fail("unexpected exception", __FILE__, __LINE__, error.what());
        } catch (...) {
            fail("unexpected non-standard exception", __FILE__, __LINE__, {});
        }

        if (failure_count_ == failures_before) {
            std::cout << "[PASS] " << current_case_ << '\n';
        }
    }

    bool check(bool condition, std::string_view expression,
               std::string_view file, int line) {
        ++check_count_;
        if (!condition) {
            fail(expression, file, line, {});
        }
        return condition;
    }

    int finish() const {
        if (failure_count_ == 0) {
            std::cout << "PASS: " << case_count_ << " cases, " << check_count_
                      << " checks\n";
            return 0;
        }
        std::cerr << "FAIL: " << failure_count_ << " failures across "
                  << case_count_ << " cases and " << check_count_ << " checks\n";
        return 1;
    }

private:
    void fail(std::string_view expression, std::string_view file, int line,
              std::string_view detail) {
        ++failure_count_;
        std::cerr << "[FAIL] " << current_case_ << " at " << file << ':' << line
                  << " -- " << expression;
        if (!detail.empty()) {
            std::cerr << ": " << detail;
        }
        std::cerr << '\n';
    }

    std::string current_case_;
    std::size_t case_count_{0};
    std::size_t check_count_{0};
    std::size_t failure_count_{0};
};

inline core::Uuid make_uuid(std::uint8_t discriminator) {
    core::Uuid::Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(discriminator + index);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    return core::Uuid::from_bytes(bytes);
}

inline core::InstallId make_install_id(std::uint8_t discriminator) {
    auto result = core::InstallId::from_uuid(make_uuid(discriminator));
    return std::move(result).value();
}

inline core::MessageId make_message_id(std::uint8_t discriminator) {
    auto result = core::MessageId::from_uuid(make_uuid(discriminator));
    return std::move(result).value();
}

} // namespace lora::test

#define CHECK(condition) \
    static_cast<void>(runner.check(static_cast<bool>(condition), #condition, __FILE__, __LINE__))

#define REQUIRE(condition) \
    if (!runner.check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)) return

#define CHECK_EQ(left, right) CHECK((left) == (right))
#define CHECK_NE(left, right) CHECK((left) != (right))
