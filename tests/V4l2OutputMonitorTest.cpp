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

#include "V4l2OutputMonitor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace floral::codec {
namespace {

class Notifications {
public:
  bool Notify() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    changed_.notify_all();
    return true;
  }

  bool Wait(unsigned count, int timeoutMs = 1000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&] { return count_ >= count; });
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  unsigned count_ = 0;
};

TEST(V4l2OutputMonitorTest, NotifiesOnceUntilDrainAcknowledges) {
  android::base::unique_fd ready(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  ASSERT_GE(ready.get(), 0);
  Notifications notifications;
  V4l2OutputMonitor monitor;
  ASSERT_TRUE(monitor.Start(ready.get(), [&] { return notifications.Notify(); }));
  const uint64_t value = 1;
  ASSERT_EQ(sizeof(value), write(ready.get(), &value, sizeof(value)));
  ASSERT_TRUE(notifications.Wait(1));
  // Keep the descriptor readable: the monitor must not enqueue a busy loop.
  EXPECT_FALSE(notifications.Wait(2, 30));
  ASSERT_TRUE(monitor.Rearm());
  ASSERT_TRUE(notifications.Wait(2));
  EXPECT_FALSE(notifications.Wait(3, 30));
  monitor.Stop();
}

TEST(V4l2OutputMonitorTest, RearmDuringCallbackDoesNotLoseReadiness) {
  android::base::unique_fd ready(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  Notifications notifications;
  V4l2OutputMonitor monitor;
  std::atomic<unsigned> count{0};
  ASSERT_TRUE(monitor.Start(ready.get(), [&] {
    if (++count == 1) {
      EXPECT_TRUE(monitor.Rearm());
    }
    return notifications.Notify();
  }));
  const uint64_t value = 1;
  ASSERT_EQ(sizeof(value), write(ready.get(), &value, sizeof(value)));
  ASSERT_TRUE(notifications.Wait(2));
  EXPECT_FALSE(notifications.Wait(3, 30));
  monitor.Stop();
}

TEST(V4l2OutputMonitorTest, StopsIdleAndDisarmedWaitsAndRestarts) {
  android::base::unique_fd ready(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  Notifications notifications;
  V4l2OutputMonitor monitor;
  ASSERT_TRUE(monitor.Start(ready.get(), [&] { return notifications.Notify(); }));
  monitor.Stop();
  const uint64_t value = 1;
  ASSERT_EQ(sizeof(value), write(ready.get(), &value, sizeof(value)));
  ASSERT_TRUE(monitor.Start(ready.get(), [&] { return notifications.Notify(); }));
  ASSERT_TRUE(notifications.Wait(1));
  monitor.Stop();
  EXPECT_FALSE(notifications.Wait(2, 30));
  ASSERT_TRUE(monitor.Start(ready.get(), [&] { return notifications.Notify(); }));
  ASSERT_TRUE(notifications.Wait(2));
  monitor.Stop();
}

TEST(V4l2OutputMonitorTest, ReportsHangupAndCleansUpFailedStart) {
  V4l2OutputMonitor monitor;
  EXPECT_FALSE(monitor.Start(-1, [] { return true; }));
  int descriptors[2];
  ASSERT_EQ(0, pipe2(descriptors, O_CLOEXEC | O_NONBLOCK));
  android::base::unique_fd reader(descriptors[0]);
  android::base::unique_fd writer(descriptors[1]);
  Notifications notifications;
  ASSERT_TRUE(monitor.Start(reader.get(), [&] { return notifications.Notify(); }));
  writer.reset();
  ASSERT_TRUE(notifications.Wait(1));
  EXPECT_EQ(EIO, monitor.error());
  EXPECT_FALSE(notifications.Wait(2, 30));
  monitor.Stop();
}

} // namespace
} // namespace floral::codec
