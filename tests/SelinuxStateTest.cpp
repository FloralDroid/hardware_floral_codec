// Copyright 2026 FloralDroid
// SPDX-License-Identifier: Apache-2.0
// Test the real adapter, not just its mode-selection table.
#include "SandboxMode.h"
#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>
#include <linux/magic.h>
#include <sys/vfs.h>

namespace {
int leafError, parentError, queryError, enforce, queries;
long leafType, parentType;
class SelinuxReaderTest : public testing::Test {
  void SetUp() override {
    leafError = parentError = queryError = queries = 0;
    leafType = SELINUX_MAGIC;
    parentType = SYSFS_MAGIC;
    enforce = 1;
  }
};
}  // namespace

extern "C" int statfs(const char* path, struct statfs* result) noexcept {
  const bool leaf = std::strcmp(path, "/sys/fs/selinux") == 0;
  const int error = leaf ? leafError : parentError;
  if (error) { errno = error; return -1; }
  result->f_type = leaf ? leafType : parentType;
  return 0;
}
extern "C" int security_getenforce() {
  ++queries;
  errno = queryError;
  return enforce;
}
using namespace floral::codec;

TEST_F(SelinuxReaderTest, StatesAndErrorsRemainDistinct) {
  EXPECT_EQ(SelinuxState::Enforcing, QuerySelinuxState().state);
  enforce = 0;
  EXPECT_EQ(SelinuxState::Permissive, QuerySelinuxState().state);
  enforce = -1;
  for (int error : {EACCES, ENOENT, EIO, 0}) {
    queryError = error;
    const auto status = QuerySelinuxState();
    EXPECT_EQ(SelinuxState::Unknown, status.state);
    EXPECT_EQ(error ? error : EIO, status.error);
  }
}
TEST_F(SelinuxReaderTest, FailedStatCannotDisable) {
  for (int error : {EACCES, EIO, ENOTDIR}) {
    leafError = error;
    EXPECT_EQ(SelinuxState::Unknown, QuerySelinuxState().state);
    EXPECT_EQ(error, QuerySelinuxState().error);
  }
  EXPECT_EQ(0, queries);
}
TEST_F(SelinuxReaderTest, OnlyExpectedSysfsConfirmsUnmounted) {
  leafType = SYSFS_MAGIC;
  EXPECT_EQ(SelinuxState::Disabled, QuerySelinuxState().state);
  leafType = TMPFS_MAGIC;
  EXPECT_EQ(SelinuxState::Unknown, QuerySelinuxState().state);
  leafError = ENOENT;
  EXPECT_EQ(SelinuxState::Disabled, QuerySelinuxState().state);
  parentError = EACCES;
  EXPECT_EQ(SelinuxState::Unknown, QuerySelinuxState().state);
  parentError = 0; parentType = TMPFS_MAGIC;
  EXPECT_EQ(SelinuxState::Unknown, QuerySelinuxState().state);
  EXPECT_EQ(0, queries);
}
