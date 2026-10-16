/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string_view>

namespace floral::codec {

enum class SandboxMode { Disabled, Enabled, Invalid };
enum class SelinuxState { Unknown, Disabled, Permissive, Enforcing };

struct SelinuxStatus {
  SelinuxState state = SelinuxState::Unknown;
  int error = 0;
};

// A query failure is not evidence that SELinux is disabled.
SelinuxStatus QuerySelinuxState();

// Missing configuration preserves legacy disabled-SELinux development, but
// defaults to the sandbox on enforcing systems. Unknown values never disable it.
inline SandboxMode ResolveSandboxMode(std::string_view value, SelinuxState state) {
  if (value == "0") {
    return SandboxMode::Disabled;
  }
  if (value == "1") {
    return state == SelinuxState::Enforcing ? SandboxMode::Enabled : SandboxMode::Invalid;
  }
  if (value.empty()) {
    switch (state) {
      case SelinuxState::Enforcing: return SandboxMode::Enabled;
      case SelinuxState::Disabled:
      case SelinuxState::Permissive: return SandboxMode::Disabled;
      case SelinuxState::Unknown: return SandboxMode::Invalid;
    }
  }
  return SandboxMode::Invalid;
}

bool SetUpServiceSandbox();

}  // namespace floral::codec
