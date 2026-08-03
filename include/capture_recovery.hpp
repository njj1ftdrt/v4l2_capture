#pragma once

#include "camera_capture_error.hpp"

#include <stdexcept>

// Pure policy object: it contains no V4L2 calls and can therefore be tested
// without a camera or /dev/video* device.
class CaptureRecoveryController {
public:
    CaptureRecoveryController(int timeout_threshold, int max_recovery_attempts)
        : timeout_threshold_(timeout_threshold),
          max_recovery_attempts_(max_recovery_attempts) {
        if (timeout_threshold_ <= 0) {
            throw std::invalid_argument("timeout_threshold must be positive");
        }
        if (max_recovery_attempts_ <= 0) {
            throw std::invalid_argument("max_recovery_attempts must be positive");
        }
    }

    bool should_reinitialize(CameraCaptureErrorKind kind) noexcept {
        switch (kind) {
            case CameraCaptureErrorKind::Timeout:
                ++consecutive_timeouts_;
                return consecutive_timeouts_ >= timeout_threshold_;
            case CameraCaptureErrorKind::TemporaryUnavailable:
                return false;
            case CameraCaptureErrorKind::DeviceUnavailable:
            case CameraCaptureErrorKind::StreamFailure:
            case CameraCaptureErrorKind::InvalidBufferMetadata:
                return true;
        }
        return true;
    }

    bool begin_recovery_attempt() noexcept {
        if (recovery_attempts_ >= max_recovery_attempts_) {
            return false;
        }
        ++recovery_attempts_;
        return true;
    }

    void on_capture_success() noexcept {
        consecutive_timeouts_ = 0;
        recovery_attempts_ = 0;
    }

    void on_recovery_success() noexcept {
        consecutive_timeouts_ = 0;
        recovery_attempts_ = 0;
    }

    int consecutive_timeouts() const noexcept { return consecutive_timeouts_; }
    int recovery_attempts() const noexcept { return recovery_attempts_; }
    int timeout_threshold() const noexcept { return timeout_threshold_; }
    int max_recovery_attempts() const noexcept { return max_recovery_attempts_; }

private:
    int timeout_threshold_;
    int max_recovery_attempts_;
    int consecutive_timeouts_{0};
    int recovery_attempts_{0};
};

enum class InvalidFrameDecision {
    None,
    Recover,
    SuppressedByCooldown,
    SuppressedByBudget,
};

// Tracks bursts of frame-validation failures independently from ioctl errors.
// A valid frame resets the streak. Recovery is rate-limited by a post-recovery
// cooldown and a per-process budget so a noisy virtualized camera cannot cause
// a stream-rebuild storm.
class InvalidFrameRecoveryController {
public:
    InvalidFrameRecoveryController(
        int threshold,
        int cooldown_frames,
        int max_recoveries
    )
        : threshold_(threshold),
          cooldown_frames_(cooldown_frames),
          max_recoveries_(max_recoveries) {
        if (threshold_ <= 0) {
            throw std::invalid_argument("invalid-frame threshold must be positive");
        }
        if (cooldown_frames_ < 0) {
            throw std::invalid_argument("invalid-frame cooldown must be non-negative");
        }
        if (max_recoveries_ <= 0) {
            throw std::invalid_argument("invalid-frame recovery budget must be positive");
        }
    }

    InvalidFrameDecision observe(bool frame_valid) noexcept {
        if (cooldown_remaining_ > 0) {
            --cooldown_remaining_;
        }

        if (frame_valid) {
            consecutive_invalid_ = 0;
            return InvalidFrameDecision::None;
        }

        ++consecutive_invalid_;
        if (consecutive_invalid_ > peak_invalid_) {
            peak_invalid_ = consecutive_invalid_;
        }

        if (consecutive_invalid_ < threshold_) {
            return InvalidFrameDecision::None;
        }

        consecutive_invalid_ = 0;

        if (cooldown_remaining_ > 0) {
            ++suppressed_by_cooldown_;
            return InvalidFrameDecision::SuppressedByCooldown;
        }
        if (recoveries_completed_ >= max_recoveries_) {
            ++suppressed_by_budget_;
            return InvalidFrameDecision::SuppressedByBudget;
        }
        return InvalidFrameDecision::Recover;
    }

    void on_invalid_frame_recovery_success() noexcept {
        consecutive_invalid_ = 0;
        cooldown_remaining_ = cooldown_frames_;
        ++recoveries_completed_;
    }

    void on_external_recovery_success() noexcept {
        consecutive_invalid_ = 0;
        cooldown_remaining_ = cooldown_frames_;
    }

    int consecutive_invalid() const noexcept { return consecutive_invalid_; }
    int peak_invalid() const noexcept { return peak_invalid_; }
    int threshold() const noexcept { return threshold_; }
    int cooldown_remaining() const noexcept { return cooldown_remaining_; }
    int recoveries_completed() const noexcept { return recoveries_completed_; }
    int max_recoveries() const noexcept { return max_recoveries_; }
    int suppressed_by_cooldown() const noexcept { return suppressed_by_cooldown_; }
    int suppressed_by_budget() const noexcept { return suppressed_by_budget_; }

private:
    int threshold_;
    int cooldown_frames_;
    int max_recoveries_;
    int consecutive_invalid_{0};
    int peak_invalid_{0};
    int cooldown_remaining_{0};
    int recoveries_completed_{0};
    int suppressed_by_cooldown_{0};
    int suppressed_by_budget_{0};
};
