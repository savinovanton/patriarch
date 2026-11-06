#pragma once

#include "pat/core/assert.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace pat::core {

/// Default error payload: a stable numeric code plus a human-readable message.
struct Error {
    int code = 0;
    std::string message;

    [[nodiscard]] bool operator==(const Error&) const = default;
};

/// Value-or-error type (C++20 stand-in for std::expected). No exceptions are thrown;
/// accessing the wrong alternative is a programming error caught by PAT_ASSERT.
template <class T, class E = Error>
class [[nodiscard]] Result {
public:
    [[nodiscard]] static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    [[nodiscard]] static Result err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & {
        PAT_ASSERT(has_value(), "Result::value() on an error");
        return *std::get_if<0>(&data_);
    }
    [[nodiscard]] const T& value() const& {
        PAT_ASSERT(has_value(), "Result::value() on an error");
        return *std::get_if<0>(&data_);
    }
    [[nodiscard]] T&& value() && {
        PAT_ASSERT(has_value(), "Result::value() on an error");
        return std::move(*std::get_if<0>(&data_));
    }

    [[nodiscard]] const E& error() const& {
        PAT_ASSERT(!has_value(), "Result::error() on a value");
        return *std::get_if<1>(&data_);
    }

    template <class U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return has_value() ? *std::get_if<0>(&data_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    Result(std::in_place_index_t<0> tag, T value) : data_(tag, std::move(value)) {}
    Result(std::in_place_index_t<1> tag, E error) : data_(tag, std::move(error)) {}

    std::variant<T, E> data_;
};

/// Specialization for operations that return nothing on success.
template <class E>
class [[nodiscard]] Result<void, E> {
public:
    [[nodiscard]] static Result ok() { return Result(); }
    [[nodiscard]] static Result err(E error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const E& error() const& {
        PAT_ASSERT(!has_value(), "Result::error() on success");
        return *error_;
    }

private:
    Result() = default;
    explicit Result(E error) : error_(std::move(error)) {}

    std::optional<E> error_;
};

}  // namespace pat::core
