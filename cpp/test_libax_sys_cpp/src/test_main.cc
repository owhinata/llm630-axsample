#include <gtest/gtest.h>

#include "axsys/sys.hpp"

namespace {

class AxsysEnv : public ::testing::Environment {
 public:
  void SetUp() override {
    sys_ = new axsys::System();
    ASSERT_TRUE(sys_->Ok()) << "AX_SYS_Init failed";
  }
  void TearDown() override {
    delete sys_;
    sys_ = nullptr;
  }

 private:
  axsys::System* sys_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new AxsysEnv());
  return RUN_ALL_TESTS();
}
