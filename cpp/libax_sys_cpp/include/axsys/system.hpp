/**
 * @file system.hpp
 * @brief C++11 RAII interface for AX_SYS lifecycle.
 *
 * Use System to initialize AX_SYS on construction and deinitialize on
 * destruction. This prevents leaks and guarantees balanced calls.
 *
 * @code{.cpp}
 * axsys::System sys;
 * if (!sys.Ok()) { return -1; }
 * // ... use AX_SYS or axsys wrappers ...
 * @endcode
 */
#pragma once

#include <stdint.h>

#include <atomic>

namespace axsys {

/**
 * @brief RAII wrapper for AX_SYS_Init/AX_SYS_Deinit.
 * @note Non-copyable, movable.
 */
class System {
 public:
  System();
  ~System();
  System(const System&) = delete;
  System& operator=(const System&) = delete;
  System(System&& other) noexcept;
  System& operator=(System&& other) noexcept;

  /** @return true if AX_SYS_Init succeeded. */
  bool Ok() const;

 private:
  std::atomic<bool> ok_;
};

}  // namespace axsys
