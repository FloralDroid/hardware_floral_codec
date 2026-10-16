/*
 * Copyright 2026 FloralDroid
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SandboxMode.h"

#include <cerrno>
#include <linux/magic.h>
#include <selinux/selinux.h>
#include <sys/vfs.h>

namespace floral::codec {

SelinuxStatus QuerySelinuxState() {
  struct statfs fs {};
  if (statfs("/sys/fs/selinux", &fs) != 0) {
    const int error = errno;
    // Disabled spaces may not have this sysfs directory. Only an actual sysfs
    // parent confirms that case; ENOENT alone (or a denied stat) cannot disable.
    if (error != ENOENT || statfs("/sys/fs", &fs) != 0) {
      return {SelinuxState::Unknown, error == ENOENT ? errno : error};
    }
    if (static_cast<unsigned long>(fs.f_type) != SYSFS_MAGIC) {
      return {SelinuxState::Unknown, ENOENT};
    }
    return {SelinuxState::Disabled, 0};
  }
  if (static_cast<unsigned long>(fs.f_type) == SYSFS_MAGIC) {
    return {SelinuxState::Disabled, 0};
  }
  if (static_cast<unsigned long>(fs.f_type) != SELINUX_MAGIC) {
    return {SelinuxState::Unknown, EINVAL};
  }
  errno = 0;
  const int enforcing = security_getenforce();
  if (enforcing == 1) return {SelinuxState::Enforcing, 0};
  if (enforcing == 0) return {SelinuxState::Permissive, 0};
  return {SelinuxState::Unknown, errno ? errno : EIO};
}

}  // namespace floral::codec
