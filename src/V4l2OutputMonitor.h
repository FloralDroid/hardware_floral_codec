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

#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

namespace floral::codec {

// Observe capture readiness only. All dequeue and Codec2 work completion stay
// on the component's existing work thread, after process() publishes its work.
class V4l2OutputMonitor {
public:
  ~V4l2OutputMonitor() { Stop(); }

  bool Start(int fd, std::function<bool()> requestDrain) {
    if (thread_.joinable()) {
      return true;
    }
    device_.reset(fcntl(fd, F_DUPFD_CLOEXEC, 0));
    if (device_.get() < 0) {
      return false;
    }
    wake_.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (wake_.get() < 0) {
      device_.reset();
      return false;
    }
    stopping_ = false;
    error_ = 0;
    thread_ = std::thread([this, requestDrain = std::move(requestDrain)] {
      bool armed = true;
      while (!stopping_) {
        pollfd descriptors[] = {{wake_.get(), POLLIN, 0},
                                {armed ? device_.get() : -1, POLLIN, 0}};
        int result;
        do {
          result = poll(descriptors, 2, -1);
        } while (result < 0 && errno == EINTR && !stopping_);
        if (stopping_) {
          break;
        }
        if (result < 0) {
          error_ = errno;
        } else if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          error_ = EIO;
        }
        if (error_ != 0) {
          (void)requestDrain();
          break;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
          uint64_t value;
          ssize_t bytes;
          do {
            bytes = read(wake_.get(), &value, sizeof(value));
          } while (bytes < 0 && errno == EINTR);
          armed = true;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
          // A readable capture fd remains readable until the component drains
          // it. Queue at most one notification and wait for its acknowledgement.
          armed = false;
          if (!requestDrain()) {
            break;
          }
        }
      }
    });
    return true;
  }

  bool Rearm() {
    if (wake_.get() < 0) {
      return true;
    }
    const uint64_t value = 1;
    ssize_t bytes;
    do {
      bytes = write(wake_.get(), &value, sizeof(value));
    } while (bytes < 0 && errno == EINTR);
    return bytes == static_cast<ssize_t>(sizeof(value)) ||
           (bytes < 0 && errno == EAGAIN);
  }

  void Stop() {
    stopping_ = true;
    (void)Rearm();
    if (thread_.joinable()) {
      thread_.join();
    }
    device_.reset();
    wake_.reset();
    error_ = 0;
  }

  int error() const { return error_; }

private:
  android::base::unique_fd device_;
  android::base::unique_fd wake_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> error_{0};
};

} // namespace floral::codec
