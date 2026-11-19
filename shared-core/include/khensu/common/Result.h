#pragma once
#include <cassert>
#include <string>
#include <utility>

namespace khensu {

/// Lightweight Result<T, E> - avoids exceptions in non-RT code paths.
/// Use is_ok() / is_err() before calling value() / error().
/// THREAD_CONTEXT(AnyThread) - value type, safe everywhere once constructed.
template <typename T, typename E = std::string> class Result {
  public:
    static Result ok(T val)
    {
        Result r;
        r.m_value = std::move(val);
        r.m_ok = true;
        return r;
    }

    static Result err(E error)
    {
        Result r;
        r.m_error = std::move(error);
        r.m_ok = false;
        return r;
    }

    bool is_ok() const noexcept { return m_ok; }
    bool is_err() const noexcept { return !m_ok; }

    /// The success value. Calling it on an error result is a programming
    /// error (CORE-04): it asserts in Debug, and in Release still returns a
    /// value-initialised T - which silently turned every score weight into 0
    /// on a DB error. Use value_or() when an error has a sensible fallback.
    const T& value() const&
    {
        assert(m_ok && "Result::value() called on an error result - check is_ok() or use value_or()");
        return m_value;
    }
    T& value() &
    {
        assert(m_ok && "Result::value() called on an error result - check is_ok() or use value_or()");
        return m_value;
    }
    /// The success value, or `fallback` when this is an error result.
    template <typename U> T value_or(U&& fallback) const&
    {
        return m_ok ? m_value : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U> T value_or(U&& fallback) &&
    {
        return m_ok ? std::move(m_value) : static_cast<T>(std::forward<U>(fallback));
    }
    const E& error() const& { return m_error; }

  private:
    T m_value{};
    E m_error{};
    bool m_ok = false;
};

/// Specialisation for void - used for operations that succeed or return an error string.
template <typename E> class Result<void, E> {
  public:
    static Result ok()
    {
        Result r;
        r.m_ok = true;
        return r;
    }
    static Result err(E error)
    {
        Result r;
        r.m_error = std::move(error);
        r.m_ok = false;
        return r;
    }

    bool is_ok() const noexcept { return m_ok; }
    bool is_err() const noexcept { return !m_ok; }
    const E& error() const { return m_error; }

  private:
    E m_error{};
    bool m_ok = false;
};

} // namespace khensu
