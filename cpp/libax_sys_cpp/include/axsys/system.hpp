// C++11 interface for AX_SYS lifecycle
#pragma once

#include <stdint.h>

namespace axsys {

// RAII wrapper for AX_SYS_Init/AX_SYS_Deinit.
// - Constructor calls AX_SYS_Init() and records success state.
// - Destructor calls AX_SYS_Deinit() if initialized.
// - Non-copyable, movable.
class System {
 public:
  System();
  ~System();
  System(const System&) = delete;
  System& operator=(const System&) = delete;
  System(System&& other) noexcept;
  System& operator=(System&& other) noexcept;

  bool Ok() const;  // true if init succeeded

 private:
  bool ok_;
};

}  // namespace axsys
