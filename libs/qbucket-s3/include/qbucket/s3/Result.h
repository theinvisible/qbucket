#pragma once

#include <qbucket/s3/Error.h>

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace qbucket::s3 {

// Result<T>: either a value or an Error — the library's only error channel
// (no exceptions). Member names follow std::expected so a later move to C++23
// is mechanical.
//
// Accessing value() on an error (or error() on a value) is a programming
// error: it asserts in debug builds and is undefined behaviour otherwise.
//
// Value type; reentrant.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    using value_type = T;

    Result(const T& value) : m_data(std::in_place_index<0>, value) {}
    Result(T&& value) : m_data(std::in_place_index<0>, std::move(value)) {}
    Result(const Error& error) : m_data(std::in_place_index<1>, error) {}
    Result(Error&& error) : m_data(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return m_data.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() &
    {
        Q_ASSERT(has_value());
        return *std::get_if<0>(&m_data);
    }
    [[nodiscard]] const T& value() const&
    {
        Q_ASSERT(has_value());
        return *std::get_if<0>(&m_data);
    }
    [[nodiscard]] T&& value() &&
    {
        Q_ASSERT(has_value());
        return std::move(*std::get_if<0>(&m_data));
    }

    [[nodiscard]] const Error& error() const&
    {
        Q_ASSERT(!has_value());
        return *std::get_if<1>(&m_data);
    }
    [[nodiscard]] Error&& error() &&
    {
        Q_ASSERT(!has_value());
        return std::move(*std::get_if<1>(&m_data));
    }

    template <typename U>
    [[nodiscard]] T value_or(U&& fallback) const&
    {
        return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    // Maps the value with f; an error passes through unchanged.
    template <typename F>
    [[nodiscard]] auto transform(F&& f) const& -> Result<std::invoke_result_t<F, const T&>>
    {
        using U = std::invoke_result_t<F, const T&>;
        if (!has_value())
            return Result<U>(error());
        if constexpr (std::is_void_v<U>) {
            std::forward<F>(f)(value());
            return Result<U>();
        } else {
            return Result<U>(std::forward<F>(f)(value()));
        }
    }

private:
    std::variant<T, Error> m_data;
};

// Result<void>: success or an Error.
template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    Result() = default;
    Result(const Error& error) : m_error(error) {}
    Result(Error&& error) : m_error(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return !m_error.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const { Q_ASSERT(has_value()); }

    [[nodiscard]] const Error& error() const&
    {
        Q_ASSERT(!has_value());
        return *m_error;
    }
    [[nodiscard]] Error&& error() &&
    {
        Q_ASSERT(!has_value());
        return std::move(*m_error);
    }

private:
    std::optional<Error> m_error;
};

} // namespace qbucket::s3
