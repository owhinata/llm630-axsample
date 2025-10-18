#include "axsys/system.hpp"

#include <ax_sys_api.h>
#include <stdio.h>

namespace axsys {

System::System() : ok_(AX_SYS_Init() == 0) {
  if (!ok_) {
    printf("AX_SYS_Init failed\n");
  }
}

System::~System() {
  if (ok_.load()) {
    AX_SYS_Deinit();
  }
}

System::System(System&& other) noexcept : ok_(other.ok_.load()) {
  other.ok_.store(false);
}

System& System::operator=(System&& other) noexcept {
  if (this != &other) {
    if (ok_.load()) {
      AX_SYS_Deinit();
    }
    ok_.store(other.ok_.load());
    other.ok_.store(false);
  }
  return *this;
}

bool System::Ok() const { return ok_.load(); }

}  // namespace axsys
