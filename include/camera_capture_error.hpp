#pragma once

#include <stdexcept>
#include <string>

// Capture-path failures are classified so the pipeline can distinguish
// transient conditions from failures that require rebuilding the V4L2 stream.
enum class CameraCaptureErrorKind {
    Timeout,
    TemporaryUnavailable,
    DeviceUnavailable,
    StreamFailure,
    InvalidBufferMetadata
};

inline const char* camera_capture_error_kind_name(CameraCaptureErrorKind kind) noexcept {
    switch (kind) {
        case CameraCaptureErrorKind::Timeout:
            return "timeout";
        case CameraCaptureErrorKind::TemporaryUnavailable:
            return "temporary_unavailable";
        case CameraCaptureErrorKind::DeviceUnavailable:
            return "device_unavailable";
        case CameraCaptureErrorKind::StreamFailure:
            return "stream_failure";
        case CameraCaptureErrorKind::InvalidBufferMetadata:
            return "invalid_buffer_metadata";
    }
    return "unknown";
}

class CameraCaptureError : public std::runtime_error {
public:
    CameraCaptureError(
        CameraCaptureErrorKind kind,
        int error_code,
        const std::string& message
    )
        : std::runtime_error(message),
          kind_(kind),
          error_code_(error_code) {}

    CameraCaptureErrorKind kind() const noexcept {
        return kind_;
    }

    int error_code() const noexcept {
        return error_code_;
    }

private:
    CameraCaptureErrorKind kind_;
    int error_code_;
};
