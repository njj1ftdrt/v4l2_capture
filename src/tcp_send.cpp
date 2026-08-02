#include "tcp_send.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace tcp_io {
namespace {

bool cancellation_requested(const std::atomic<bool>* stop_requested) noexcept {
    return stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed);
}

std::runtime_error socket_error(const std::string& prefix, int error_number) {
    return std::runtime_error(prefix + ": " + std::strerror(error_number));
}

int remaining_wait_ms(
    SendClock::time_point deadline,
    std::chrono::milliseconds poll_slice
) {
    const auto now = SendClock::now();
    if (now >= deadline) {
        return 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto bounded = std::min(
        poll_slice,
        remaining.count() > 0 ? remaining : std::chrono::milliseconds(1)
    );

    const auto count = bounded.count();
    return count > static_cast<long long>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(count);
}

[[noreturn]] void throw_timeout(std::size_t sent, std::size_t size) {
    throw SendTimeout(
        "TCP send timed out after sending " + std::to_string(sent) +
        "/" + std::to_string(size) + " bytes"
    );
}

[[noreturn]] void throw_cancelled(std::size_t sent, std::size_t size) {
    throw SendCancelled(
        "TCP send cancelled after sending " + std::to_string(sent) +
        "/" + std::to_string(size) + " bytes"
    );
}

}  // namespace

void send_all_until(
    int fd,
    const void* data,
    std::size_t size,
    SendClock::time_point deadline,
    const std::atomic<bool>* stop_requested,
    std::chrono::milliseconds poll_slice
) {
    if (fd < 0) {
        throw std::invalid_argument("TCP send requires a valid socket fd");
    }

    if (size > 0 && data == nullptr) {
        throw std::invalid_argument("TCP send received a null data pointer");
    }

    if (poll_slice.count() <= 0) {
        throw std::invalid_argument("TCP send poll slice must be positive");
    }

    const auto* ptr = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        if (cancellation_requested(stop_requested)) {
            throw_cancelled(sent, size);
        }

        if (SendClock::now() >= deadline) {
            throw_timeout(sent, size);
        }

        const ssize_t n = ::send(
            fd,
            ptr + sent,
            size - sent,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL |
#endif
#ifdef MSG_DONTWAIT
            MSG_DONTWAIT
#else
            0
#endif
        );

        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }

        if (n == 0) {
            throw std::runtime_error("send returned 0, peer may have closed connection");
        }

        const int send_errno = errno;
        if (send_errno == EINTR) {
            continue;
        }

        if (send_errno != EAGAIN && send_errno != EWOULDBLOCK) {
            throw socket_error("send failed", send_errno);
        }

        if (cancellation_requested(stop_requested)) {
            throw_cancelled(sent, size);
        }

        const int wait_ms = remaining_wait_ms(deadline, poll_slice);
        if (wait_ms <= 0) {
            throw_timeout(sent, size);
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;

        const int poll_result = ::poll(&pfd, 1, wait_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("poll(POLLOUT) failed", errno);
        }

        if (poll_result == 0) {
            continue;
        }

        if ((pfd.revents & POLLNVAL) != 0) {
            throw std::runtime_error("poll(POLLOUT) reported POLLNVAL");
        }

        if ((pfd.revents & (POLLERR | POLLHUP)) != 0) {
            int socket_error_number = 0;
            socklen_t length = sizeof(socket_error_number);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error_number, &length) == 0 &&
                socket_error_number != 0) {
                throw socket_error("socket became unavailable while sending", socket_error_number);
            }
            throw std::runtime_error("socket became unavailable while sending");
        }
    }
}

}  // namespace tcp_io
