// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Error and Result conventions (01 section 6). No exceptions cross a library boundary: every fallible public
// function in layers 0-3 returns Result<T>. Result is an own C++20 class (never std::expected, which is
// C++23).
//
// Reading value() of a failed Result, or error() of a successful one, is a programming error and terminates
// through ESSES_FATAL in every build type (there is no value to return).
#pragma once

#include <esses/base/assert.hpp>
#include <esses/base/config.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace esses {

// Values mirror the C ABI's esses_status 1:1 (D-051, D-131); never renumber.
// NOLINTNEXTLINE(performance-enum-size): the width of the C ABI's status, with room to grow.
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    Io,          // open/read/write/mmap failure; context.path
    Format,      // malformed or unexpected file content; context.path, context.offset
    Unsupported, // valid but not implemented (dtype, version, feature)
    NotFound,    // channel, range, file, schema, importer id
    Invalid,     // bad argument, schema validation failure; context.detail
    Cancelled,   // CancelToken fired
    OutOfBudget, // ChunkCache pinned bytes would exceed the budget
    OutOfMemory, // allocation failure
    Cycle,       // maths dependency cycle; context.detail = "A -> B -> A"
    Parse,       // expression or profile parse error; context.span
    Internal     // invariant violated; always a bug
};

// "Io", "Format", ... : the stable leading token of every message (01 section 6.2 rule 6).
std::string_view errorCodeName(ErrorCode code) noexcept;

// Half-open byte offsets into an expression or profile; maths::Span aliases it.
struct TextSpan {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    friend constexpr bool operator==(const TextSpan&, const TextSpan&) = default;
};

struct ErrorContext {
    std::optional<std::filesystem::path> path;
    std::optional<std::uint64_t> offset;
    std::optional<std::string> channel;
    std::optional<std::string> layer;
    std::optional<std::string> importer;
    std::optional<std::string> schemaId;
    std::optional<std::string> detail;
    std::optional<TextSpan> span;
};

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message; // developer-readable, English, no trailing period
    ErrorContext context;
    // "Format: unexpected channel header at 0x1F3A (bathurst_r1.ld)"
    std::string toString() const;
};

std::ostream& operator<<(std::ostream& os, const Error& e);

struct Failure { // type-less failure; converts implicitly to any Result<T>
    Error error;
};

template <class T> class Result;

namespace detail {
[[noreturn]] void badResultAccess(const Error* error, const char* what) noexcept;

template <class T> struct IsResult : std::false_type {};
template <class T> struct IsResult<Result<T>> : std::true_type {};
template <class T> inline constexpr bool kIsResult = IsResult<std::remove_cvref_t<T>>::value;
} // namespace detail

template <class T> class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Failure>, "Result<Failure> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is not supported");

public:
    using value_type = T;

    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {} // NOLINT(google-explicit-constructor)
    template <class U>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, T> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Result> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Failure>)
    Result(U&& value)
        : v_(std::in_place_index<0>, std::forward<U>(value)) {} // NOLINT(google-explicit-constructor)
    Result(Failure f)
        : v_(std::in_place_index<1>, std::move(f.error)) {} // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & {
        check();
        return *std::get_if<0>(&v_);
    }
    const T& value() const& {
        check();
        return *std::get_if<0>(&v_);
    }
    T&& value() && {
        check();
        return std::move(*std::get_if<0>(&v_));
    }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const Error& error() const {
        if (ok()) {
            detail::badResultAccess(nullptr, "Result::error() on a successful Result");
        }
        return *std::get_if<1>(&v_);
    }

    template <class U> T valueOr(U&& fallback) const& {
        return ok() ? *std::get_if<0>(&v_) : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U> T valueOr(U&& fallback) && {
        return ok() ? std::move(*std::get_if<0>(&v_)) : static_cast<T>(std::forward<U>(fallback));
    }

    // map: F(const T&) -> U gives Result<U> (U may be void).
    template <class F> auto map(F&& f) const& {
        using U = std::invoke_result_t<F, const T&>;
        if (!ok()) {
            return Result<U>(Failure{error()});
        }
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), *std::get_if<0>(&v_));
            return Result<U>();
        } else {
            return Result<U>(std::invoke(std::forward<F>(f), *std::get_if<0>(&v_)));
        }
    }
    template <class F> auto map(F&& f) && {
        using U = std::invoke_result_t<F, T&&>;
        if (!ok()) {
            return Result<U>(Failure{std::move(*std::get_if<1>(&v_))});
        }
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), std::move(*std::get_if<0>(&v_)));
            return Result<U>();
        } else {
            return Result<U>(std::invoke(std::forward<F>(f), std::move(*std::get_if<0>(&v_))));
        }
    }

    // andThen: F(const T&) -> Result<U>.
    template <class F> auto andThen(F&& f) const& {
        using R = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
        static_assert(detail::kIsResult<R>, "andThen: the function must return a Result");
        if (!ok()) {
            return R(Failure{error()});
        }
        return std::invoke(std::forward<F>(f), *std::get_if<0>(&v_));
    }
    template <class F> auto andThen(F&& f) && {
        using R = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        static_assert(detail::kIsResult<R>, "andThen: the function must return a Result");
        if (!ok()) {
            return R(Failure{std::move(*std::get_if<1>(&v_))});
        }
        return std::invoke(std::forward<F>(f), std::move(*std::get_if<0>(&v_)));
    }

    // orElse: F(const Error&) -> Result<T>, called only on failure.
    template <class F> Result orElse(F&& f) const& {
        if (ok()) {
            return *this;
        }
        return std::invoke(std::forward<F>(f), *std::get_if<1>(&v_));
    }
    template <class F> Result orElse(F&& f) && {
        if (ok()) {
            return std::move(*this);
        }
        return std::invoke(std::forward<F>(f), *std::get_if<1>(&v_));
    }

    // 01 section 6.1 spells the monadic helpers map, and_then, or_else; both spellings are provided.
    template <class F> auto and_then(F&& f) const& { // NOLINT(readability-identifier-naming)
        return andThen(std::forward<F>(f));
    }
    template <class F> auto and_then(F&& f) && { // NOLINT(readability-identifier-naming)
        return std::move(*this).andThen(std::forward<F>(f));
    }
    template <class F> Result or_else(F&& f) const& { // NOLINT(readability-identifier-naming)
        return orElse(std::forward<F>(f));
    }
    template <class F> Result or_else(F&& f) && { // NOLINT(readability-identifier-naming)
        return std::move(*this).orElse(std::forward<F>(f));
    }

private:
    void check() const {
        if (!ok()) {
            detail::badResultAccess(std::get_if<1>(&v_), "Result::value() on a failed Result");
        }
    }

    std::variant<T, Error> v_;
};

template <> class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    Result() noexcept = default;                    // success
    Result(Failure f) : err_(std::move(f.error)) {} // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return !err_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    void value() const {
        if (err_) {
            detail::badResultAccess(&*err_, "Result<void>::value() on a failed Result");
        }
    }
    const Error& error() const {
        if (!err_) {
            detail::badResultAccess(nullptr, "Result::error() on a successful Result");
        }
        return *err_;
    }

    template <class F> auto map(F&& f) const {
        using U = std::invoke_result_t<F>;
        if (!ok()) {
            return Result<U>(Failure{*err_});
        }
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f));
            return Result<void>();
        } else {
            return Result<U>(std::invoke(std::forward<F>(f)));
        }
    }
    template <class F> auto andThen(F&& f) const {
        using R = std::remove_cvref_t<std::invoke_result_t<F>>;
        static_assert(detail::kIsResult<R>, "andThen: the function must return a Result");
        if (!ok()) {
            return R(Failure{*err_});
        }
        return std::invoke(std::forward<F>(f));
    }
    template <class F> Result orElse(F&& f) const {
        if (ok()) {
            return *this;
        }
        return std::invoke(std::forward<F>(f), *err_);
    }
    template <class F> auto and_then(F&& f) const { // NOLINT(readability-identifier-naming)
        return andThen(std::forward<F>(f));
    }
    template <class F> Result or_else(F&& f) const { // NOLINT(readability-identifier-naming)
        return orElse(std::forward<F>(f));
    }

private:
    std::optional<Error> err_;
};

// ok(v) decays, so ok(lvalue) is Result<X>, not Result<X&>.
template <class T> Result<std::decay_t<T>> ok(T&& v) {
    return Result<std::decay_t<T>>(std::forward<T>(v));
}
inline Result<void> ok() {
    return {};
}

// Deducible form: `return fail(ErrorCode::Io, "cannot open");`
inline Failure fail(ErrorCode code, std::string message, ErrorContext context = {}) {
    return Failure{Error{code, std::move(message), std::move(context)}};
}
// Explicit form where no target type is deduced: `fail<int>(ErrorCode::Invalid, "...")`.
template <class T> Result<T> fail(ErrorCode code, std::string message, ErrorContext context = {}) {
    return Failure{Error{code, std::move(message), std::move(context)}};
}

} // namespace esses
