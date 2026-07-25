/*
 * SPDX-License-Identifier: MIT
 */

#include "app_script_parser.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using platform::script::ActionType;
using platform::script::NamedKey;
using platform::script::ParseError;

class Runner {
public:
    template<typename Test>
    void run(std::string_view name, Test&& test) {
        ++case_count_;
        current_case_.assign(name);
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
        if (failure_count_ == 0U) {
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

#define CHECK(condition) \
    static_cast<void>(runner.check(static_cast<bool>(condition), #condition, __FILE__, __LINE__))

#define REQUIRE(condition) \
    if (!runner.check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)) return

std::string repeat(char character, std::size_t count) {
    return std::string(count, character);
}

std::string repeat_token(const std::string& token, std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result += token;
    }
    return result;
}

} // namespace

int main() {
    Runner runner;

    runner.run("empty environment value has no actions", [&] {
        const auto result = platform::script::parse("");
        CHECK(result);
        CHECK(result.actions.empty());
    });

    runner.run("legacy tokens remain case insensitive", [&] {
        const auto result = platform::script::parse(
            " WAIT , home , eSc , SHOT=phase0-home , CLOSE");
        REQUIRE(result);
        REQUIRE(result.actions.size() == 5U);
        CHECK(result.actions[0].type == ActionType::Wait);
        CHECK(result.actions[1].named_key == NamedKey::Home);
        CHECK(result.actions[2].named_key == NamedKey::Escape);
        CHECK(result.actions[3].type == ActionType::Screenshot);
        CHECK(result.actions[3].value == "phase0-home");
        CHECK(result.actions[4].type == ActionType::CloseWindow);
    });

    runner.run("navigation and shortcut keys map deterministically", [&] {
        const auto result = platform::script::parse(
            "UP,DOWN,LEFT,RIGHT,ENTER,BACKSPACE,TAB,N,R,M,S,D");
        REQUIRE(result);
        REQUIRE(result.actions.size() == 12U);
        CHECK(result.actions[0].named_key == NamedKey::Up);
        CHECK(result.actions[1].named_key == NamedKey::Down);
        CHECK(result.actions[2].named_key == NamedKey::Left);
        CHECK(result.actions[3].named_key == NamedKey::Right);
        CHECK(result.actions[4].named_key == NamedKey::Enter);
        CHECK(result.actions[5].named_key == NamedKey::Backspace);
        CHECK(result.actions[6].named_key == NamedKey::Tab);
        CHECK(result.actions[7].scalar == static_cast<std::uint32_t>('n'));
        CHECK(result.actions[8].scalar == static_cast<std::uint32_t>('r'));
        CHECK(result.actions[9].scalar == static_cast<std::uint32_t>('m'));
        CHECK(result.actions[10].scalar == static_cast<std::uint32_t>('s'));
        CHECK(result.actions[11].scalar == static_cast<std::uint32_t>('d'));
    });

    runner.run("wait count expands at both boundaries", [&] {
        const auto minimum = platform::script::parse("WAIT=1");
        REQUIRE(minimum);
        CHECK(minimum.actions.size() == 1U);
        const auto maximum = platform::script::parse("WAIT=100");
        REQUIRE(maximum);
        CHECK(maximum.actions.size() == 100U);
        CHECK(maximum.actions.front().type == ActionType::Wait);
        CHECK(maximum.actions.back().type == ActionType::Wait);
    });

    runner.run("invalid wait counts fail closed", [&] {
        for (const auto* source : {"WAIT=", "WAIT=0", "WAIT=101", "WAIT=-1",
                                   "WAIT=1x", "WAIT=42949672960"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::InvalidValue);
        }
    });

    runner.run("percent text expands into unicode scalars", [&] {
        const auto result = platform::script::parse("TEXT=Hello%2C%20world%21");
        REQUIRE(result);
        const std::string expected = "Hello, world!";
        REQUIRE(result.actions.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK(result.actions[index].type == ActionType::Key);
            CHECK(result.actions[index].scalar ==
                  static_cast<std::uint32_t>(expected[index]));
        }
    });

    runner.run("Japanese and Simplified Chinese text decode by scalar", [&] {
        const auto japanese = platform::script::parse(
            "TEXT=%E3%83%86%E3%82%B9%E3%83%88");
        REQUIRE(japanese);
        REQUIRE(japanese.actions.size() == 3U);
        CHECK(japanese.actions[0].scalar == 0x30c6U);
        CHECK(japanese.actions[1].scalar == 0x30b9U);
        CHECK(japanese.actions[2].scalar == 0x30c8U);

        const auto chinese = platform::script::parse("TEXT=%E6%B5%8B%E8%AF%95");
        REQUIRE(chinese);
        REQUIRE(chinese.actions.size() == 2U);
        CHECK(chinese.actions[0].scalar == 0x6d4bU);
        CHECK(chinese.actions[1].scalar == 0x8bd5U);
    });

    runner.run("reserved input bytes require percent encoding", [&] {
        for (const auto* source : {"TEXT=Hello world", "TEXT=hello,world",
                                   "TEXT=100%", "TEXT==", "TEXT=raw!"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::InvalidTextEncoding ||
                  result.error == ParseError::UnknownToken);
        }
    });

    runner.run("malformed percent escapes fail", [&] {
        for (const auto* source : {"TEXT=%", "TEXT=%0", "TEXT=%GG", "TEXT=%0z"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::InvalidTextEncoding);
        }
    });

    runner.run("malformed and non-scalar UTF-8 fail", [&] {
        for (const auto* source : {"TEXT=%80", "TEXT=%C0%80", "TEXT=%E3%81",
                                   "TEXT=%ED%A0%80", "TEXT=%F4%90%80%80",
                                   "TEXT=%F5%80%80%80"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::UnsafeText);
        }
    });

    runner.run("control format and noncharacter text fail", [&] {
        for (const auto* source : {"TEXT=%00", "TEXT=%0A", "TEXT=%1F", "TEXT=%7F",
                                   "TEXT=%C2%85", "TEXT=%E2%80%AE",
                                   "TEXT=%E2%80%A8", "TEXT=%EF%B7%90",
                                   "TEXT=%EF%BF%BF"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::UnsafeText);
        }
    });

    runner.run("text decoded byte limit is exact", [&] {
        const auto maximum = platform::script::parse("TEXT=" + repeat('a', 160U));
        REQUIRE(maximum);
        CHECK(maximum.actions.size() == 160U);
        const auto excessive = platform::script::parse("TEXT=" + repeat('a', 161U));
        CHECK(!excessive);
        CHECK(excessive.error == ParseError::InvalidTextEncoding);
    });

    runner.run("expect and await retain stable field values", [&] {
        const auto result = platform::script::parse(
            "EXPECT=screen:timeline,AWAIT=modal:error,EXPECT=locale:zh-hans,"
            "EXPECT=focus:compose.body,EXPECT=status:broadcast,"
            "EXPECT=count:4,EXPECT=persistence:ready,"
            "EXPECT=newest-state:unknown");
        REQUIRE(result);
        REQUIRE(result.actions.size() == 8U);
        CHECK(result.actions[0].type == ActionType::Expect);
        CHECK(result.actions[0].field == "screen");
        CHECK(result.actions[0].value == "timeline");
        CHECK(result.actions[1].type == ActionType::Await);
        CHECK(result.actions[1].field == "modal");
        CHECK(result.actions[1].value == "error");
        CHECK(result.actions[2].field == "locale");
        CHECK(result.actions[2].value == "zh-hans");
        CHECK(result.actions[3].field == "focus");
        CHECK(result.actions[4].field == "status");
        CHECK(result.actions[5].field == "count");
        CHECK(result.actions[5].value == "4");
        CHECK(result.actions[6].field == "persistence");
        CHECK(result.actions[7].field == "newest-state");
    });

    runner.run("invalid probe specifications fail", [&] {
        for (const auto* source : {"EXPECT=screen", "EXPECT=:timeline",
                                   "EXPECT=screen:", "EXPECT=screen:Timeline",
                                   "EXPECT=screen:time:line", "EXPECT=user:alice",
                                   "AWAIT=screen:timeline!"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::InvalidValue);
        }
    });

    runner.run("safe screenshot stems are preserved exactly", [&] {
        const auto result = platform::script::parse(
            "SHOT=a,SHOT=phase2-ja-compose,SHOT=a.b_c-9");
        REQUIRE(result);
        REQUIRE(result.actions.size() == 3U);
        CHECK(result.actions[0].value == "a");
        CHECK(result.actions[1].value == "phase2-ja-compose");
        CHECK(result.actions[2].value == "a.b_c-9");
        CHECK(platform::script::is_safe_screenshot_stem("a"));
        CHECK(platform::script::is_safe_screenshot_stem(repeat('a', 64U)));
    });

    runner.run("empty screenshot remains a runtime negative-test sentinel", [&] {
        const auto result = platform::script::parse("SHOT=");
        REQUIRE(result);
        REQUIRE(result.actions.size() == 1U);
        CHECK(result.actions[0].type == ActionType::Screenshot);
        CHECK(result.actions[0].value.empty());
    });

    runner.run("unsafe screenshot stems fail without rewriting", [&] {
        for (const auto& source : {
                 std::string("SHOT=Upper"), std::string("SHOT=-leading"),
                 std::string("SHOT=trailing."), std::string("SHOT=a/b"),
                 std::string("SHOT=a%2Fb"), std::string("SHOT=") + repeat('a', 65U)}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::InvalidValue);
        }
        CHECK(!platform::script::is_safe_screenshot_stem(""));
        CHECK(!platform::script::is_safe_screenshot_stem("A"));
    });

    runner.run("empty and unknown tokens fail closed", [&] {
        for (const auto* source : {" ", ",WAIT", "WAIT,,HOME", "WAIT,", "NOPE"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
        }
        CHECK(platform::script::parse("NOPE").error == ParseError::UnknownToken);
    });

    runner.run("close is terminal", [&] {
        const auto valid = platform::script::parse("WAIT,CLOSE");
        REQUIRE(valid);
        CHECK(valid.actions.back().type == ActionType::CloseWindow);
        for (const auto* source : {"CLOSE,WAIT", "CLOSE,", "WAIT,CLOSE,SHOT=a"}) {
            const auto result = platform::script::parse(source);
            CHECK(!result);
            CHECK(result.error == ParseError::CloseMustBeLast);
        }
    });

    runner.run("source and token limits fail before unbounded expansion", [&] {
        const auto source_too_long =
            platform::script::parse(repeat('a', platform::script::kMaximumSourceBytes + 1U));
        CHECK(!source_too_long);
        CHECK(source_too_long.error == ParseError::SourceTooLong);

        const auto token_too_long =
            platform::script::parse("TEXT=" + repeat('a', 508U));
        CHECK(!token_too_long);
        CHECK(token_too_long.error == ParseError::TokenTooLong);
    });

    runner.run("expanded action limit is exact", [&] {
        auto exact_source = repeat_token("WAIT=100", 10U);
        exact_source += ",WAIT=24";
        const auto exact = platform::script::parse(exact_source);
        REQUIRE(exact);
        CHECK(exact.actions.size() == platform::script::kMaximumActions);

        const auto excessive = platform::script::parse(exact_source + ",WAIT");
        CHECK(!excessive);
        CHECK(excessive.error == ParseError::TooManyActions);
    });

    runner.run("interval parsing is strict and bounded", [&] {
        const auto unset = platform::script::parse_interval(nullptr);
        CHECK(unset.valid);
        CHECK(unset.milliseconds == platform::script::kDefaultIntervalMilliseconds);
        for (const auto* value : {"20", "200", "5000"}) {
            const auto result = platform::script::parse_interval(value);
            CHECK(result.valid);
        }
        for (const auto* value : {"", "19", "5001", "20junk", "+20", " 20",
                                  "42949672960"}) {
            const auto result = platform::script::parse_interval(value);
            CHECK(!result.valid);
            CHECK(result.milliseconds == platform::script::kDefaultIntervalMilliseconds);
        }
    });

    return runner.finish();
}
