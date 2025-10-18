/**
 * @file result.hpp
 * @brief Result<T> with lazy error message generation for axsys APIs.
 *
 * Result<T> conveys success or failure of operations. On success it
 * holds a value; on failure it carries an ErrorCode and a lazily
 * generated message. Accessing Message() materializes the message via
 * the stored factory.
 *
 * Usage example
 * @code{.cpp}
 * Result<int> r = SomeOp();
 * if (!r) { fprintf(stderr, "%s\n", r.Message().c_str()); }
 * int v = r.MoveValue();
 * @endcode
 */
#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include "axsys/error.hpp"

namespace axsys {

namespace detail {
/** Stores error code and lazily materializes message via factory. */
struct ErrorDetail {
  ErrorCode code = ErrorCode::kSuccess;
  mutable std::string message;  // materialized on first access
  mutable std::function<std::string()> message_factory;  // may be empty

  ErrorDetail() = default;
  explicit ErrorDetail(ErrorCode c) : code(c) {}
  ErrorDetail(ErrorCode c, std::function<std::string()> factory)
      : code(c), message_factory(std::move(factory)) {}

  const std::string& Message() const {
    if (message.empty() && message_factory) {
      message = message_factory();
      // release factory to free captured state
      message_factory = std::function<std::string()>();
    }
    return message;
  }
};
}  // namespace detail

/**
 * @brief Result type carrying either T or an error.
 * @tparam T Success value type.
 */
template <typename T>
class Result {
 public:
  /** Construct a successful Result with a copy of value. */
  Result(const T& value) : ok_(true), value_(value) {}
  /** Construct a successful Result moving the value. */
  Result(T&& value) : ok_(true), value_(std::move(value)) {}

  /** Construct an error Result with code and optional message factory. */
  Result(ErrorCode code, std::function<std::string()> msg_factory = {})
      : ok_(false), error_(code, std::move(msg_factory)) {}

  /** Create a success Result by moving the value. */
  static Result<T> Ok(T value) { return Result<T>(std::move(value)); }
  /** Create an error Result with code and optional message factory. */
  static Result<T> Error(ErrorCode code,
                         std::function<std::string()> msg_factory = {}) {
    return Result<T>(code, std::move(msg_factory));
  }

  explicit operator bool() const noexcept { return ok_; }

  /** @return ErrorCode::kSuccess on ok, otherwise the stored code. */
  ErrorCode Code() const noexcept {
    return ok_ ? ErrorCode::kSuccess : error_.code;
  }

  /**
   * @brief Retrieve the error message, constructing it on first use.
   * @note Returns an empty string on success.
   */
  const std::string& Message() const {
    static const std::string kEmpty;
    return ok_ ? kEmpty : error_.Message();
  }

  /** @name Value access (valid only when ok_) */
  ///@{
  T& Value() { return value_; }
  const T& Value() const { return value_; }

  T* operator->() { return &value_; }
  const T* operator->() const { return &value_; }

  T& operator*() { return value_; }
  const T& operator*() const { return value_; }

  /** Move the value out (valid only when ok_). */
  T&& MoveValue() { return std::move(value_); }
  ///@}

 private:
  bool ok_ = false;
  T value_{};  // default-initialized; only used when ok_ == true
  detail::ErrorDetail error_{};  // only used when ok_ == false
};

/** Partial specialization for void results. */
template <>
class Result<void> {
 public:
  Result() : ok_(true) {}
  /** Error result with code and optional message factory. */
  explicit Result(ErrorCode code,
                  std::function<std::string()> msg_factory = {})
      : ok_(false), error_(code, std::move(msg_factory)) {}

  static Result<void> Ok() { return Result<void>(); }
  static Result<void> Error(ErrorCode code,
                            std::function<std::string()> msg_factory = {}) {
    return Result<void>(code, std::move(msg_factory));
  }

  explicit operator bool() const noexcept { return ok_; }

  /** @return ErrorCode::kSuccess on ok, otherwise the stored code. */
  ErrorCode Code() const noexcept {
    return ok_ ? ErrorCode::kSuccess : error_.code;
  }

  /**
   * @brief Retrieve the error message, constructing it on first use.
   * @note Returns an empty string on success.
   */
  const std::string& Message() const {
    static const std::string kEmpty;
    return ok_ ? kEmpty : error_.Message();
  }

 private:
  bool ok_ = false;
  detail::ErrorDetail error_{};
};

}  // namespace axsys
