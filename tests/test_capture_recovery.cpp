#include "capture_recovery.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_timeout_threshold() {
    CaptureRecoveryController controller(3, 5);

    require(!controller.should_reinitialize(CameraCaptureErrorKind::Timeout),
            "first timeout must not rebuild the stream");
    require(!controller.should_reinitialize(CameraCaptureErrorKind::Timeout),
            "second timeout must not rebuild the stream");
    require(controller.should_reinitialize(CameraCaptureErrorKind::Timeout),
            "third consecutive timeout must rebuild the stream");
    require(controller.consecutive_timeouts() == 3,
            "timeout counter mismatch");
}

void test_success_resets_timeout_sequence() {
    CaptureRecoveryController controller(2, 3);

    require(!controller.should_reinitialize(CameraCaptureErrorKind::Timeout),
            "first timeout must be tolerated");
    controller.on_capture_success();
    require(controller.consecutive_timeouts() == 0,
            "successful capture must reset consecutive timeouts");
    require(!controller.should_reinitialize(CameraCaptureErrorKind::Timeout),
            "timeout after a successful frame must start a new sequence");
}

void test_error_classification() {
    CaptureRecoveryController controller(3, 5);

    require(!controller.should_reinitialize(
                CameraCaptureErrorKind::TemporaryUnavailable),
            "EAGAIN-like temporary unavailability must not rebuild the stream");
    require(controller.should_reinitialize(
                CameraCaptureErrorKind::DeviceUnavailable),
            "device loss must rebuild the stream immediately");
    require(controller.should_reinitialize(
                CameraCaptureErrorKind::StreamFailure),
            "stream failure must rebuild the stream immediately");
    require(controller.should_reinitialize(
                CameraCaptureErrorKind::InvalidBufferMetadata),
            "invalid buffer metadata must rebuild the stream immediately");
}

void test_recovery_attempt_budget() {
    CaptureRecoveryController controller(3, 2);

    require(controller.begin_recovery_attempt(),
            "first recovery attempt must be permitted");
    require(controller.begin_recovery_attempt(),
            "second recovery attempt must be permitted");
    require(!controller.begin_recovery_attempt(),
            "attempts beyond the configured budget must be rejected");

    controller.on_recovery_success();
    require(controller.recovery_attempts() == 0,
            "successful recovery must reset the attempt budget");
    require(controller.begin_recovery_attempt(),
            "a new incident must receive a fresh attempt budget");
}


void test_invalid_frame_threshold() {
    InvalidFrameRecoveryController controller(3, 5, 2);

    require(controller.observe(false) == InvalidFrameDecision::None,
            "first invalid frame must be dropped without rebuilding");
    require(controller.observe(false) == InvalidFrameDecision::None,
            "second invalid frame must be dropped without rebuilding");
    require(controller.observe(false) == InvalidFrameDecision::Recover,
            "third consecutive invalid frame must request a rebuild");
    require(controller.peak_invalid() == 3,
            "invalid-frame peak mismatch");
    require(controller.consecutive_invalid() == 0,
            "streak must restart after a rebuild request");
}

void test_valid_frame_resets_invalid_streak() {
    InvalidFrameRecoveryController controller(2, 0, 2);

    require(controller.observe(false) == InvalidFrameDecision::None,
            "first invalid frame must not request a rebuild");
    require(controller.observe(true) == InvalidFrameDecision::None,
            "a valid frame must not request a rebuild");
    require(controller.consecutive_invalid() == 0,
            "valid frame must reset the invalid streak");
    require(controller.observe(false) == InvalidFrameDecision::None,
            "invalid frame after a valid frame must start a new streak");
}

void test_invalid_frame_cooldown() {
    InvalidFrameRecoveryController controller(2, 4, 3);

    require(controller.observe(false) == InvalidFrameDecision::None,
            "first invalid frame must be tolerated");
    require(controller.observe(false) == InvalidFrameDecision::Recover,
            "second invalid frame must request recovery");
    controller.on_invalid_frame_recovery_success();
    require(controller.cooldown_remaining() == 4,
            "recovery success must arm cooldown");

    require(controller.observe(false) == InvalidFrameDecision::None,
            "first invalid frame during cooldown must be counted");
    require(controller.observe(false) == InvalidFrameDecision::SuppressedByCooldown,
            "threshold during cooldown must not rebuild");
    require(controller.suppressed_by_cooldown() == 1,
            "cooldown suppression count mismatch");
}

void test_invalid_frame_recovery_budget() {
    InvalidFrameRecoveryController controller(1, 0, 2);

    require(controller.observe(false) == InvalidFrameDecision::Recover,
            "first incident must request recovery");
    controller.on_invalid_frame_recovery_success();
    require(controller.observe(false) == InvalidFrameDecision::Recover,
            "second incident must request recovery");
    controller.on_invalid_frame_recovery_success();
    require(controller.observe(false) == InvalidFrameDecision::SuppressedByBudget,
            "incident beyond budget must be suppressed");
    require(controller.recoveries_completed() == 2,
            "completed recovery count mismatch");
    require(controller.suppressed_by_budget() == 1,
            "budget suppression count mismatch");
}

void test_external_recovery_does_not_consume_invalid_budget() {
    InvalidFrameRecoveryController controller(1, 3, 1);
    controller.on_external_recovery_success();
    require(controller.recoveries_completed() == 0,
            "device-error recovery must not consume invalid-frame budget");
    require(controller.cooldown_remaining() == 3,
            "external recovery must still arm cooldown");
}

void test_invalid_frame_configuration() {
    bool threshold_failed = false;
    try {
        InvalidFrameRecoveryController invalid(0, 0, 1);
    } catch (const std::invalid_argument&) {
        threshold_failed = true;
    }
    require(threshold_failed, "zero invalid-frame threshold must be rejected");

    bool cooldown_failed = false;
    try {
        InvalidFrameRecoveryController invalid(1, -1, 1);
    } catch (const std::invalid_argument&) {
        cooldown_failed = true;
    }
    require(cooldown_failed, "negative cooldown must be rejected");

    bool budget_failed = false;
    try {
        InvalidFrameRecoveryController invalid(1, 0, 0);
    } catch (const std::invalid_argument&) {
        budget_failed = true;
    }
    require(budget_failed, "zero invalid-frame recovery budget must be rejected");
}

void test_invalid_configuration() {
    bool threshold_failed = false;
    try {
        CaptureRecoveryController invalid(0, 1);
    } catch (const std::invalid_argument&) {
        threshold_failed = true;
    }
    require(threshold_failed, "zero timeout threshold must be rejected");

    bool attempts_failed = false;
    try {
        CaptureRecoveryController invalid(1, 0);
    } catch (const std::invalid_argument&) {
        attempts_failed = true;
    }
    require(attempts_failed, "zero recovery attempts must be rejected");
}

}  // namespace

int main() {
    try {
        test_timeout_threshold();
        test_success_resets_timeout_sequence();
        test_error_classification();
        test_recovery_attempt_budget();
        test_invalid_frame_threshold();
        test_valid_frame_resets_invalid_streak();
        test_invalid_frame_cooldown();
        test_invalid_frame_recovery_budget();
        test_external_recovery_does_not_consume_invalid_budget();
        test_invalid_frame_configuration();
        test_invalid_configuration();
        std::cout << "capture recovery policy tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "capture recovery policy test failed: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
