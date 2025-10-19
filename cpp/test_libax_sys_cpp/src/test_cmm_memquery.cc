#include <gtest/gtest.h>

#include "axsys/sys.hpp"

namespace {

/**
 * @brief Case025: MemQueryStatus wrapper test.
 *
 * Purpose:
 * - Exercise CmmBuffer::MemQueryStatus and perform basic sanity checks.
 * Steps:
 * - Call MemQueryStatus(&st)
 * Expected:
 * - Returns true; total_size >= remain_size.
 */
TEST(CmmMemQuery, Case025_MemQueryStatus) {
  axsys::CmmBuffer::CmmStatus st;
  ASSERT_TRUE(axsys::CmmBuffer::MemQueryStatus(&st));
  // Basic sanity checks
  EXPECT_GE(st.total_size, st.remain_size);
}

}  // namespace
