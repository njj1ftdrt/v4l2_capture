#include "tcp_send.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class SocketPair {
public:
    SocketPair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) {
            throw std::runtime_error("socketpair failed");
        }
    }

    ~SocketPair() {
        close_fd(fds_[0]);
        close_fd(fds_[1]);
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    int first() const noexcept { return fds_[0]; }
    int second() const noexcept { return fds_[1]; }

private:
    static void close_fd(int& fd) noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    int fds_[2]{-1, -1};
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void reduce_send_buffer(int fd) {
    int size = 4096;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0) {
        throw std::runtime_error("setsockopt(SO_SNDBUF) failed");
    }
}

void test_successful_send() {
    SocketPair sockets;
    std::vector<std::uint8_t> payload(256 * 1024, 0x5a);
    std::vector<std::uint8_t> received(payload.size());

    std::exception_ptr reader_error;
    std::thread reader([&]() {
        try {
            std::size_t offset = 0;
            while (offset < received.size()) {
                const ssize_t n = ::recv(
                    sockets.second(),
                    received.data() + offset,
                    received.size() - offset,
                    0
                );
                if (n <= 0) {
                    throw std::runtime_error("reader failed before receiving full payload");
                }
                offset += static_cast<std::size_t>(n);
            }
        } catch (...) {
            reader_error = std::current_exception();
        }
    });

    tcp_io::send_all_with_timeout(
        sockets.first(),
        payload.data(),
        payload.size(),
        std::chrono::seconds(2)
    );

    reader.join();
    if (reader_error != nullptr) {
        std::rethrow_exception(reader_error);
    }
    require(received == payload, "successful send payload mismatch");
}

void test_slow_peer_times_out() {
    SocketPair sockets;
    reduce_send_buffer(sockets.first());
    std::vector<std::uint8_t> payload(16 * 1024 * 1024, 0xa5);

    const auto start = std::chrono::steady_clock::now();
    bool timed_out = false;

    try {
        tcp_io::send_all_with_timeout(
            sockets.first(),
            payload.data(),
            payload.size(),
            std::chrono::milliseconds(120),
            nullptr,
            std::chrono::milliseconds(10)
        );
    } catch (const tcp_io::SendTimeout&) {
        timed_out = true;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    require(timed_out, "slow peer did not trigger SendTimeout");
    require(elapsed < std::chrono::seconds(2), "slow peer timeout was not bounded");
}

void test_send_can_be_cancelled() {
    SocketPair sockets;
    reduce_send_buffer(sockets.first());
    std::vector<std::uint8_t> payload(16 * 1024 * 1024, 0x3c);
    std::atomic<bool> stop_requested{false};
    std::exception_ptr sender_error;

    const auto start = std::chrono::steady_clock::now();
    std::thread sender([&]() {
        try {
            tcp_io::send_all_with_timeout(
                sockets.first(),
                payload.data(),
                payload.size(),
                std::chrono::seconds(5),
                &stop_requested,
                std::chrono::milliseconds(10)
            );
        } catch (...) {
            sender_error = std::current_exception();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    stop_requested.store(true);
    sender.join();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    require(sender_error != nullptr, "cancelled send did not report an exception");

    bool cancelled = false;
    try {
        std::rethrow_exception(sender_error);
    } catch (const tcp_io::SendCancelled&) {
        cancelled = true;
    }

    require(cancelled, "cancelled send did not report SendCancelled");
    require(elapsed < std::chrono::seconds(2), "send cancellation was not bounded");
}

}  // namespace

int main() {
    try {
        test_successful_send();
        test_slow_peer_times_out();
        test_send_can_be_cancelled();
        std::cout << "test_tcp_send: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_tcp_send: FAIL: " << e.what() << "\n";
        return 1;
    }
}
