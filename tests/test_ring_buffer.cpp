#include "ring_buffer.hpp"

#include <cassert>
#include <iostream>
#include <vector>

int main() {
    RingBuffer<int> buffer(3);

    assert(buffer.capacity() == 3);
    assert(buffer.size() == 0);
    assert(buffer.dropped_count() == 0);

    buffer.push(1);
    buffer.push(2);
    buffer.push(3);

    {
        std::vector<int> snapshot = buffer.snapshot();
        assert((snapshot == std::vector<int>{1, 2, 3}));
    }

    buffer.push(4);

    {
        std::vector<int> snapshot = buffer.snapshot();
        assert((snapshot == std::vector<int>{2, 3, 4}));
    }

    assert(buffer.size() == 3);
    assert(buffer.dropped_count() == 1);

    int value = 0;

    bool ok = buffer.try_pop_oldest(value);
    assert(ok);
    assert(value == 2);
    assert(buffer.size() == 2);

    ok = buffer.try_pop_oldest(value);
    assert(ok);
    assert(value == 3);

    ok = buffer.try_pop_oldest(value);
    assert(ok);
    assert(value == 4);

    ok = buffer.try_pop_oldest(value);
    assert(!ok);
    assert(buffer.size() == 0);

    std::cout << "RingBuffer tests passed.\n";
    return 0;
}
