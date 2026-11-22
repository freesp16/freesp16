// A value or an error message, for fallible non-real-time operations (file formats, store, library: FMT-*, PRJ-*).
// No exceptions cross module boundaries; errors carry a user-facing message (spec wording) and an optional detail.
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace core
{
struct Error
{
    std::string message; // shown to the user
    std::string detail {}; // log only
};

template <typename T>
class [[nodiscard]] Result
{
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}
    static Result failure(std::string message, std::string detail = {}) { return Error {std::move(message), std::move(detail)}; }

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    T& value() & { return *value_; }
    const T& value() const& { return *value_; }
    T&& value() && { return std::move(*value_); }
    const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    Error error_ {};
};

template <>
class [[nodiscard]] Result<void>
{
public:
    Result() = default;
    Result(Error error) : ok_(false), error_(std::move(error)) {}
    static Result failure(std::string message, std::string detail = {}) { return Error {std::move(message), std::move(detail)}; }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const noexcept { return error_; }

private:
    bool ok_ = true;
    Error error_ {};
};
} // namespace core
