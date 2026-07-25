/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <utility>
#include <variant>

namespace lora::core {

template<typename T, typename E>
class Result {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool has_value() const noexcept {
        return value_.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    const T& value() const& {
        return std::get<0>(value_);
    }

    T& value() & {
        return std::get<0>(value_);
    }

    T&& value() && {
        return std::get<0>(std::move(value_));
    }

    const E& error() const& {
        return std::get<1>(value_);
    }

private:
    template<typename... Args>
    explicit Result(std::in_place_index_t<0> index, Args&&... args)
        : value_(index, std::forward<Args>(args)...) {}

    template<typename... Args>
    explicit Result(std::in_place_index_t<1> index, Args&&... args)
        : value_(index, std::forward<Args>(args)...) {}

    std::variant<T, E> value_;
};

} // namespace lora::core
