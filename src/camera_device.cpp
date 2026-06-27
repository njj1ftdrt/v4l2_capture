#include "camera_device.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

CameraDevice::CameraDevice(std::string device_path)
    : device_path_(std::move(device_path)) {}

CameraDevice::~CameraDevice() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

CameraDevice::CameraDevice(CameraDevice&& other) noexcept
    : device_path_(std::move(other.device_path_)), fd_(other.fd_) {
    other.fd_ = -1;
}

CameraDevice& CameraDevice::operator=(CameraDevice&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }

        device_path_ = std::move(other.device_path_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }

    return *this;
}

void CameraDevice::open_device() {
    fd_ = ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK);

    if (fd_ < 0) {
        throw std::runtime_error(
            "Failed to open device " + device_path_ + ": " + std::strerror(errno)
        );
    }

    std::cout << "[INFO] Opened device: " << device_path_ << "\n";
}

void CameraDevice::print_capability_flag(__u32 caps, __u32 flag, const std::string& name) {
    std::cout << "  " << name << ": " << ((caps & flag) ? "yes" : "no") << "\n";
}

void CameraDevice::query_capability() const {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    v4l2_capability cap{};
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        throw std::runtime_error(
            "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno))
        );
    }

    std::cout << "========== V4L2 Capability ==========\n";
    std::cout << "driver      : " << reinterpret_cast<const char*>(cap.driver) << "\n";
    std::cout << "card        : " << reinterpret_cast<const char*>(cap.card) << "\n";
    std::cout << "bus_info    : " << reinterpret_cast<const char*>(cap.bus_info) << "\n";
    std::cout << "version     : "
              << ((cap.version >> 16) & 0xFF) << "."
              << ((cap.version >> 8) & 0xFF) << "."
              << (cap.version & 0xFF) << "\n";

    std::cout << "capabilities: 0x" << std::hex << cap.capabilities << std::dec << "\n";
    std::cout << "device_caps : 0x" << std::hex << cap.device_caps << std::dec << "\n";

    __u32 effective_caps = cap.capabilities;
    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
        effective_caps = cap.device_caps;
    }

    std::cout << "---------- Effective Capabilities ----------\n";
    print_capability_flag(effective_caps, V4L2_CAP_VIDEO_CAPTURE, "V4L2_CAP_VIDEO_CAPTURE");
    print_capability_flag(effective_caps, V4L2_CAP_VIDEO_OUTPUT, "V4L2_CAP_VIDEO_OUTPUT");
    print_capability_flag(effective_caps, V4L2_CAP_READWRITE, "V4L2_CAP_READWRITE");
    print_capability_flag(effective_caps, V4L2_CAP_STREAMING, "V4L2_CAP_STREAMING");

    if (!(effective_caps & V4L2_CAP_VIDEO_CAPTURE)) {
        std::cout << "[WARN] This device does not report VIDEO_CAPTURE capability.\n";
        std::cout << "[WARN] If this is v4l2loopback, start ffmpeg producer first and retry.\n";
    }

    if (!(effective_caps & V4L2_CAP_STREAMING)) {
        std::cout << "[WARN] This device does not support STREAMING I/O.\n";
    }

    std::cout << "=====================================\n";
}
