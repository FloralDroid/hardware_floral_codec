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

#define LOG_TAG "android.hardware.media.c2@1.2-service-floral"

#include "floral/codec/FloralCodecStore.h"

#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <signal.h>

int main() {
  using ::android::hardware::media::c2::V1_2::IComponentStore;
  using ::android::hardware::media::c2::V1_2::utils::ComponentStore;

  signal(SIGPIPE, SIG_IGN);
  ::android::ProcessState::initWithDriver("/dev/vndbinder");
  ::android::ProcessState::self()->startThreadPool();
  ::android::hardware::configureRpcThreadpool(8, true);

  ::android::sp<IComponentStore> store =
      new ComponentStore(floral::codec::FloralCodecStore::Create());
  if (store == nullptr ||
      store->registerAsService("default") != ::android::OK) {
    ALOGE("failed to register the Floral Codec2 component store");
    return 1;
  }
  ALOGI("Floral Codec2 component store registered");
  ::android::hardware::joinRpcThreadpool();
  return 0;
}
