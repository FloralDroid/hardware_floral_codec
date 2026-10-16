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

#define LOG_TAG "FloralCodecSandbox"

#include "SandboxMode.h"

#include <android-base/properties.h>
#include <libminijail.h>
#include <linux/seccomp.h>
#include <log/log.h>
#include <scoped_minijail.h>
#include <sys/prctl.h>

namespace floral::codec {

bool SetUpServiceSandbox() {
  const auto value = android::base::GetProperty("ro.boot.floral_codec_seccomp", "");
  // An explicit development switch must not depend on privileged state reads.
  SelinuxStatus status;
  if (value.empty() || value == "1") {
    status = QuerySelinuxState();
  }
  const auto mode = ResolveSandboxMode(value, status.state);
  if (mode == SandboxMode::Invalid) {
    ALOGE("Invalid codec sandbox configuration or unknown SELinux state (errno=%d)", status.error);
    return false;
  }
  if (mode == SandboxMode::Disabled) {
    ALOGI("Codec seccomp disabled: %s", value == "0" ? "explicit development flag"
                                                   : "confirmed disabled/permissive SELinux");
    return true;
  }

  // Install before probing drivers, creating stores or starting Binder threads.
  // A missing/invalid mandatory policy is fatal; there is no permissive fallback.
  ScopedMinijail jail{minijail_new()};
  if (!jail) {
    ALOGE("Cannot allocate codec sandbox");
    return false;
  }
  minijail_no_new_privs(jail.get());
  minijail_use_seccomp_filter(jail.get());
  minijail_set_seccomp_filter_tsync(jail.get());
  // Do NOT request minijail_log_seccomp_filter_failures: on kernels supporting
  // SECCOMP_RET_LOG it can log and ALLOW forbidden calls, including debug builds.
  minijail_parse_seccomp_filters(jail.get(), "/vendor/etc/seccomp_policy/floral-codec.policy");
  minijail_enter(jail.get());
  if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
      prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != SECCOMP_MODE_FILTER) {
    ALOGE("Codec sandbox did not become active");
    return false;
  }
  ALOGI("Codec seccomp sandbox active");
  return true;
}

}  // namespace floral::codec
