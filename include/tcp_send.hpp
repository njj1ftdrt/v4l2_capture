#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace tcp_io {

class SendTimeout final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SendCancelled final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using SendClock = std::chrono::steady_clock;

void send_all_until(
    int fd,
    const void* data,
    std::size_t size,
    SendClock::time_point deadline,
    const std::atomic<bool>* stop_requested = nullptr,
    std::chrono::milliseconds poll_slice = std::chrono::milliseconds(50)
);

inline void send_all_with_timeout(
    int fd,
    const void* data,
    std::size_t size,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* stop_requested = nullptr,
    std::chrono::milliseconds poll_slice = std::chrono::milliseconds(50)
) {
    if (timeout.count() <= 0) {
        throw std::invalid_argument("TCP send timeout must be positive");
    }

    send_all_until(
        fd,
        data,
        size,
        SendClock::now() + timeout,
        stop_requested,
        poll_slice
    );
}

}  // namespace tcp_io
