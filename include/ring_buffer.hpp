#pragma once

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
        : buffer_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        push_locked(item);
    }

    void push(T&& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        push_locked(std::move(item));
    }

    bool try_pop_oldest(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (size_ == 0) {
            return false;
        }

        out = std::move(buffer_[read_index_]);
        read_index_ = (read_index_ + 1) % buffer_.size();
        --size_;
        return true;
    }

    std::vector<T> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<T> result;
        result.reserve(size_);

        for (std::size_t i = 0; i < size_; ++i) {
            const std::size_t index = (read_index_ + i) % buffer_.size();
            result.push_back(buffer_[index]);
        }

        return result;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    std::size_t capacity() const {
        return buffer_.size();
    }

    std::size_t dropped_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

private:
    template <typename U>
    void push_locked(U&& item) {
        buffer_[write_index_] = std::forward<U>(item);

        if (size_ == buffer_.size()) {
            read_index_ = (read_index_ + 1) % buffer_.size();
            ++dropped_count_;
        } else {
            ++size_;
        }

        write_index_ = (write_index_ + 1) % buffer_.size();
    }

    mutable std::mutex mutex_;
    std::vector<T> buffer_;

    std::size_t read_index_{0};
    std::size_t write_index_{0};
    std::size_t size_{0};
    std::size_t dropped_count_{0};
};
