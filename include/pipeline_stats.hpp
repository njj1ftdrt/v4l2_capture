#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Centralized runtime statistics for the V4L2 + TCP pipeline.
// Keep this intentionally lightweight and dependency-free.
// It is designed for:
// 1. final log output,
// 2. future end-to-end latency statistics,
// 3. future ROS2 status publisher integration.

struct PipelineStatsSnapshot {
    std::uint64_t captured = 0;
    std::uint64_t consumed = 0;
    std::uint64_t invalid = 0;
    std::uint64_t saved = 0;
    std::uint64_t consumed_bytes = 0;

    std::uint64_t capture_dropped = 0;
    std::uint64_t capture_queue_remaining = 0;

    std::uint64_t tcp_queued = 0;
    std::uint64_t tcp_dropped = 0;
    std::uint64_t tcp_queue_remaining = 0;
    std::uint64_t tcp_sent = 0;
    std::uint64_t tcp_sent_bytes = 0;
    std::uint64_t tcp_send_errors = 0;

    // Reserved for tcp_receiver or future ROS2/status aggregation.
    std::uint64_t received = 0;
    std::uint64_t reconnect_count = 0;

    double elapsed_seconds = 0.0;
    double producer_fps = 0.0;
    double consumer_fps = 0.0;

    std::string last_error;
};

class PipelineStats {
public:
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> invalid{0};
    std::atomic<std::uint64_t> saved{0};
    std::atomic<std::uint64_t> consumed_bytes{0};

    std::atomic<std::uint64_t> capture_dropped{0};
    std::atomic<std::uint64_t> capture_queue_remaining{0};

    std::atomic<std::uint64_t> tcp_queued{0};
    std::atomic<std::uint64_t> tcp_dropped{0};
    std::atomic<std::uint64_t> tcp_queue_remaining{0};
    std::atomic<std::uint64_t> tcp_sent{0};
    std::atomic<std::uint64_t> tcp_sent_bytes{0};
    std::atomic<std::uint64_t> tcp_send_errors{0};

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> reconnect_count{0};

    void set_last_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = message;
    }

    std::string last_error() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

    PipelineStatsSnapshot snapshot(double elapsed_seconds) const {
        PipelineStatsSnapshot s;
        s.captured = captured.load();
        s.consumed = consumed.load();
        s.invalid = invalid.load();
        s.saved = saved.load();
        s.consumed_bytes = consumed_bytes.load();
        s.capture_dropped = capture_dropped.load();
        s.capture_queue_remaining = capture_queue_remaining.load();
        s.tcp_queued = tcp_queued.load();
        s.tcp_dropped = tcp_dropped.load();
        s.tcp_queue_remaining = tcp_queue_remaining.load();
        s.tcp_sent = tcp_sent.load();
        s.tcp_sent_bytes = tcp_sent_bytes.load();
        s.tcp_send_errors = tcp_send_errors.load();
        s.received = received.load();
        s.reconnect_count = reconnect_count.load();
        s.elapsed_seconds = elapsed_seconds;
        s.producer_fps = elapsed_seconds > 0.0
            ? static_cast<double>(s.captured) / elapsed_seconds
            : 0.0;
        s.consumer_fps = elapsed_seconds > 0.0
            ? static_cast<double>(s.consumed) / elapsed_seconds
            : 0.0;
        s.last_error = last_error();
        return s;
    }

private:
    mutable std::mutex error_mutex_;
    std::string last_error_;
};
