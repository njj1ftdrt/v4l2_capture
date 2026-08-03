#include "ring_buffer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        RingBuffer<int> buffer(3);

        require(buffer.capacity() == 3, "unexpected capacity");
        require(buffer.size() == 0, "new buffer must be empty");
        require(buffer.dropped_count() == 0, "new buffer must not report drops");

        buffer.push(1);
        buffer.push(2);
        buffer.push(3);

        {
            const auto snapshot = buffer.snapshot();
            require(snapshot == std::vector<int>{1, 2, 3}, "initial FIFO order mismatch");
        }

        buffer.push(4);

        {
            const auto snapshot = buffer.snapshot();
            require(snapshot == std::vector<int>{2, 3, 4}, "drop-oldest order mismatch");
        }

        require(buffer.size() == 3, "full buffer size mismatch");
        require(buffer.dropped_count() == 1, "drop counter mismatch");

        int value = 0;
        bool ok = buffer.try_pop_oldest(value);
        require(ok, "first pop unexpectedly failed");
        require(value == 2, "first popped value mismatch");
        require(buffer.size() == 2, "size after first pop mismatch");

        ok = buffer.try_pop_oldest(value);
        require(ok, "second pop unexpectedly failed");
        require(value == 3, "second popped value mismatch");

        ok = buffer.try_pop_oldest(value);
        require(ok, "third pop unexpectedly failed");
        require(value == 4, "third popped value mismatch");

        ok = buffer.try_pop_oldest(value);
        require(!ok, "empty buffer pop unexpectedly succeeded");
        require(buffer.size() == 0, "buffer must be empty after all pops");

        std::cout << "test_ring_buffer: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_ring_buffer: FAIL: " << e.what() << "\n";
        return 1;
    }
}
