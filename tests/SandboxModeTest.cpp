/*
 * Copyright 2026 FloralDroid
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *      http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SandboxMode.h"

#include <gtest/gtest.h>

namespace floral::codec {

TEST(SandboxModeTest, DevelopmentDoesNotInstallAnInnerFilter) {
  for (auto state : {SelinuxState::Unknown, SelinuxState::Disabled,
                     SelinuxState::Permissive, SelinuxState::Enforcing}) {
    EXPECT_EQ(SandboxMode::Disabled, ResolveSandboxMode("0", state));
  }
  EXPECT_EQ(SandboxMode::Disabled, ResolveSandboxMode("", SelinuxState::Disabled));
  EXPECT_EQ(SandboxMode::Disabled, ResolveSandboxMode("", SelinuxState::Permissive));
}

TEST(SandboxModeTest, EnforcingDefaultsToASandbox) {
  EXPECT_EQ(SandboxMode::Enabled, ResolveSandboxMode("", SelinuxState::Enforcing));
  EXPECT_EQ(SandboxMode::Enabled, ResolveSandboxMode("1", SelinuxState::Enforcing));
}

TEST(SandboxModeTest, InvalidOrInconsistentStrictConfigurationFailsClosed) {
  EXPECT_EQ(SandboxMode::Invalid, ResolveSandboxMode("", SelinuxState::Unknown));
  for (auto state : {SelinuxState::Unknown, SelinuxState::Disabled, SelinuxState::Permissive}) {
    EXPECT_EQ(SandboxMode::Invalid, ResolveSandboxMode("1", state));
  }
  for (auto state : {SelinuxState::Unknown, SelinuxState::Disabled,
                     SelinuxState::Permissive, SelinuxState::Enforcing}) {
    EXPECT_EQ(SandboxMode::Invalid, ResolveSandboxMode("false", state));
    EXPECT_EQ(SandboxMode::Invalid, ResolveSandboxMode("garbage", state));
  }
}

}  // namespace floral::codec
