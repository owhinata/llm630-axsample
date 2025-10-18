#include <gtest/gtest.h>

#include "axsys/sys.hpp"

namespace {

/*
 * Case025: MemQueryStatus wrapper test.
 * Purpose:
 * - Exercise CmmBuffer::MemQueryStatus and perform basic sanity checks.
 */
TEST(CmmMemQuery, Case025_MemQueryStatus) {
  axsys::CmmBuffer::CmmStatus st;
  ASSERT_TRUE(axsys::CmmBuffer::MemQueryStatus(&st));
  // Basic sanity checks
  EXPECT_GE(st.total_size, st.remain_size);
}

}  // namespace
