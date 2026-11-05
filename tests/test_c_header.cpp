// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

extern "C" int vnet_c_header_selftest(void);

TEST(CHeader, UsableFromPlainC11) { EXPECT_EQ(vnet_c_header_selftest(), 0); }
