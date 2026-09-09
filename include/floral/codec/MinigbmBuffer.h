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

#include <cstdint>

#include <cutils/native_handle.h>
#include <va/va_drmcommon.h>

namespace floral::codec {

// Converts minigbm's native handle to the DMA-BUF description consumed by
// VA-API. File descriptors in the output remain owned by |handle|.
bool GetMinigbmVaDescriptor(const native_handle_t *handle, uint32_t width,
                            uint32_t height,
                            VADRMPRIMESurfaceDescriptor *descriptor,
                            uint32_t *vaFourcc, uint64_t *bufferId);

}  // namespace floral::codec
