// Result<T> with lazy error message generation for axsys C++ API
#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include "axsys/error.hpp"

namespace axsys {

namespace detail {
// Stores error code and lazily materializes message via factory on demand.
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

// Primary template
template <typename T>
class Result {
 public:
  // Success constructors
  Result(const T& value) : ok_(true), value_(value) {}
  Result(T&& value) : ok_(true), value_(std::move(value)) {}

  // Error constructors
  Result(ErrorCode code, std::function<std::string()> msg_factory = {})
      : ok_(false), error_(code, std::move(msg_factory)) {}

  // Factory helpers
  static Result<T> Ok(T value) { return Result<T>(std::move(value)); }
  static Result<T> Error(ErrorCode code,
                         std::function<std::string()> msg_factory = {}) {
    return Result<T>(code, std::move(msg_factory));
  }

  explicit operator bool() const noexcept { return ok_; }

  // Accessors
  ErrorCode Code() const noexcept {
    return ok_ ? ErrorCode::kSuccess : error_.code;
  }

  const std::string& Message() const {
    static const std::string kEmpty;
    return ok_ ? kEmpty : error_.Message();
  }

  // Value access (only valid when ok_)
  T& Value() { return value_; }
  const T& Value() const { return value_; }

  T* operator->() { return &value_; }
  const T* operator->() const { return &value_; }

  T& operator*() { return value_; }
  const T& operator*() const { return value_; }

  // Move the value out (only valid when ok_)
  T&& MoveValue() { return std::move(value_); }

 private:
  bool ok_ = false;
  T value_{};  // default-initialized; only used when ok_ == true
  detail::ErrorDetail error_{};  // only used when ok_ == false
};

// Partial specialization for void
template <>
class Result<void> {
 public:
  Result() : ok_(true) {}
  explicit Result(ErrorCode code,
                  std::function<std::string()> msg_factory = {})
      : ok_(false), error_(code, std::move(msg_factory)) {}

  static Result<void> Ok() { return Result<void>(); }
  static Result<void> Error(ErrorCode code,
                            std::function<std::string()> msg_factory = {}) {
    return Result<void>(code, std::move(msg_factory));
  }

  explicit operator bool() const noexcept { return ok_; }

  ErrorCode Code() const noexcept {
    return ok_ ? ErrorCode::kSuccess : error_.code;
  }

  const std::string& Message() const {
    static const std::string kEmpty;
    return ok_ ? kEmpty : error_.Message();
  }

 private:
  bool ok_ = false;
  detail::ErrorDetail error_{};
};

}  // namespace axsys
