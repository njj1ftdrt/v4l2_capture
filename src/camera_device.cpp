#include "camera_device.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
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

std::string CameraDevice::fourcc_to_string(__u32 pixelformat) {
    std::string s;
    s.push_back(static_cast<char>(pixelformat & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 8) & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 16) & 0xFF));
    s.push_back(static_cast<char>((pixelformat >> 24) & 0xFF));
    return s;
}

__u32 CameraDevice::string_to_fourcc(const std::string& fourcc) {
    if (fourcc.size() != 4) {
        throw std::runtime_error("Pixel format must be a 4-character FourCC, for example YUYV or MJPG");
    }

    std::string normalized = fourcc;
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );

    return static_cast<__u32>(normalized[0])
        | (static_cast<__u32>(normalized[1]) << 8)
        | (static_cast<__u32>(normalized[2]) << 16)
        | (static_cast<__u32>(normalized[3]) << 24);
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
    }

    if (!(effective_caps & V4L2_CAP_STREAMING)) {
        std::cout << "[WARN] This device does not support STREAMING I/O.\n";
    }

    std::cout << "=====================================\n";
}

void CameraDevice::list_formats() const {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    std::cout << "========== Supported Pixel Formats ==========\n";

    bool found = false;

    for (__u32 index = 0;; ++index) {
        v4l2_fmtdesc fmt{};
        fmt.index = index;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (::ioctl(fd_, VIDIOC_ENUM_FMT, &fmt) < 0) {
            if (errno == EINVAL) {
                break;
            }

            throw std::runtime_error(
                "VIDIOC_ENUM_FMT failed: " + std::string(std::strerror(errno))
            );
        }

        found = true;

        std::string fourcc = fourcc_to_string(fmt.pixelformat);

        std::cout << "[" << index << "] "
                  << fourcc
                  << " - "
                  << reinterpret_cast<const char*>(fmt.description);

        if (fmt.flags & V4L2_FMT_FLAG_COMPRESSED) {
            std::cout << " (compressed)";
        }

        if (fmt.flags & V4L2_FMT_FLAG_EMULATED) {
            std::cout << " (emulated)";
        }

        std::cout << "\n";

        list_frame_sizes(fmt.pixelformat);
    }

    if (!found) {
        std::cout << "[WARN] No capture pixel format found.\n";
        std::cout << "[HINT] For v4l2loopback, keep ffmpeg producer running and retry.\n";
    }

    std::cout << "=============================================\n";
}

void CameraDevice::list_frame_sizes(__u32 pixelformat) const {
    bool found = false;

    for (__u32 index = 0;; ++index) {
        v4l2_frmsizeenum size{};
        size.index = index;
        size.pixel_format = pixelformat;

        if (::ioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
            if (errno == EINVAL) {
                break;
            }

            std::cout << "  [WARN] VIDIOC_ENUM_FRAMESIZES failed for "
                      << fourcc_to_string(pixelformat)
                      << ": "
                      << std::strerror(errno)
                      << "\n";
            return;
        }

        found = true;

        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            std::cout << "  size[" << index << "]: "
                      << size.discrete.width << "x"
                      << size.discrete.height << "\n";

            list_frame_intervals(
                pixelformat,
                size.discrete.width,
                size.discrete.height
            );
        } else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            std::cout << "  size: stepwise "
                      << size.stepwise.min_width << "x" << size.stepwise.min_height
                      << " -> "
                      << size.stepwise.max_width << "x" << size.stepwise.max_height
                      << " step "
                      << size.stepwise.step_width << "x" << size.stepwise.step_height
                      << "\n";
            break;
        } else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            std::cout << "  size: continuous "
                      << size.stepwise.min_width << "x" << size.stepwise.min_height
                      << " -> "
                      << size.stepwise.max_width << "x" << size.stepwise.max_height
                      << "\n";
            break;
        }
    }

    if (!found) {
        std::cout << "  [WARN] No frame size info available for "
                  << fourcc_to_string(pixelformat)
                  << "\n";
    }
}

void CameraDevice::list_frame_intervals(__u32 pixelformat, __u32 width, __u32 height) const {
    bool found = false;

    for (__u32 index = 0;; ++index) {
        v4l2_frmivalenum interval{};
        interval.index = index;
        interval.pixel_format = pixelformat;
        interval.width = width;
        interval.height = height;

        if (::ioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) {
            if (errno == EINVAL) {
                break;
            }

            std::cout << "    [WARN] VIDIOC_ENUM_FRAMEINTERVALS failed: "
                      << std::strerror(errno)
                      << "\n";
            return;
        }

        found = true;

        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            const auto num = interval.discrete.numerator;
            const auto den = interval.discrete.denominator;

            double fps = 0.0;
            if (num != 0) {
                fps = static_cast<double>(den) / static_cast<double>(num);
            }

            std::cout << "    interval[" << index << "]: "
                      << num << "/" << den
                      << " s"
                      << " (" << fps << " fps)"
                      << "\n";
        } else if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
            std::cout << "    interval: stepwise\n";
            break;
        } else if (interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            std::cout << "    interval: continuous\n";
            break;
        }
    }

    if (!found) {
        std::cout << "    [WARN] No frame interval info available\n";
    }
}

void CameraDevice::set_format(__u32 width, __u32 height, const std::string& pixel_format) const {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    const __u32 requested_fourcc = string_to_fourcc(pixel_format);

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = requested_fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    std::cout << "========== Set Format ==========\n";
    std::cout << "requested   : "
              << width << "x" << height
              << " " << fourcc_to_string(requested_fourcc)
              << "\n";

    if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        throw std::runtime_error(
            "VIDIOC_S_FMT failed: " + std::string(std::strerror(errno))
        );
    }

    std::cout << "accepted    : "
              << fmt.fmt.pix.width << "x" << fmt.fmt.pix.height
              << " " << fourcc_to_string(fmt.fmt.pix.pixelformat)
              << "\n";
    std::cout << "bytesperline: " << fmt.fmt.pix.bytesperline << "\n";
    std::cout << "sizeimage   : " << fmt.fmt.pix.sizeimage << "\n";
    std::cout << "colorspace  : " << fmt.fmt.pix.colorspace << "\n";

    if (fmt.fmt.pix.width != width ||
        fmt.fmt.pix.height != height ||
        fmt.fmt.pix.pixelformat != requested_fourcc) {
        std::cout << "[WARN] Driver adjusted the requested format.\n";
    }

    v4l2_format current{};
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (::ioctl(fd_, VIDIOC_G_FMT, &current) < 0) {
        throw std::runtime_error(
            "VIDIOC_G_FMT failed: " + std::string(std::strerror(errno))
        );
    }

    std::cout << "---------- Current Format ----------\n";
    std::cout << "current     : "
              << current.fmt.pix.width << "x" << current.fmt.pix.height
              << " " << fourcc_to_string(current.fmt.pix.pixelformat)
              << "\n";
    std::cout << "bytesperline: " << current.fmt.pix.bytesperline << "\n";
    std::cout << "sizeimage   : " << current.fmt.pix.sizeimage << "\n";
    std::cout << "================================\n";
}
