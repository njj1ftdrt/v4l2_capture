#include "camera_device.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

CameraDevice::CameraDevice(std::string device_path)
    : device_path_(std::move(device_path)) {}

CameraDevice::~CameraDevice() {
    stop_streaming();
    release_mmap_buffers();
    close_device();
}

CameraDevice::CameraDevice(CameraDevice&& other) noexcept
    : device_path_(std::move(other.device_path_)),
      fd_(other.fd_),
      buffers_(std::move(other.buffers_)),
      streaming_(other.streaming_),
      current_width_(other.current_width_),
      current_height_(other.current_height_),
      current_pixelformat_(other.current_pixelformat_),
      current_sizeimage_(other.current_sizeimage_) {
    other.fd_ = -1;
    other.streaming_ = false;
    other.buffers_.clear();
    other.current_width_ = 0;
    other.current_height_ = 0;
    other.current_pixelformat_ = 0;
    other.current_sizeimage_ = 0;
}

CameraDevice& CameraDevice::operator=(CameraDevice&& other) noexcept {
    if (this != &other) {
        stop_streaming();
        release_mmap_buffers();
        close_device();

        device_path_ = std::move(other.device_path_);
        fd_ = other.fd_;
        buffers_ = std::move(other.buffers_);
        streaming_ = other.streaming_;
        current_width_ = other.current_width_;
        current_height_ = other.current_height_;
        current_pixelformat_ = other.current_pixelformat_;
        current_sizeimage_ = other.current_sizeimage_;

        other.fd_ = -1;
        other.streaming_ = false;
        other.buffers_.clear();
        other.current_width_ = 0;
        other.current_height_ = 0;
        other.current_pixelformat_ = 0;
        other.current_sizeimage_ = 0;
    }

    return *this;
}

CameraDevice::DequeuedBufferGuard::DequeuedBufferGuard(
    CameraDevice& camera,
    __u32 index
) noexcept
    : camera_(&camera),
      index_(index),
      active_(true) {}

CameraDevice::DequeuedBufferGuard::~DequeuedBufferGuard() noexcept {
    if (active_ && camera_ != nullptr) {
        camera_->try_requeue_buffer_noexcept(index_);
    }
}

CameraDevice::DequeuedBufferGuard::DequeuedBufferGuard(
    DequeuedBufferGuard&& other
) noexcept
    : camera_(other.camera_),
      index_(other.index_),
      active_(other.active_) {
    other.camera_ = nullptr;
    other.active_ = false;
}

void CameraDevice::DequeuedBufferGuard::requeue_or_throw() {
    if (!active_ || camera_ == nullptr) {
        return;
    }

    active_ = false;
    camera_->requeue_buffer(index_);
}

void CameraDevice::close_device() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void CameraDevice::release_mmap_buffers() {
    for (auto& buffer : buffers_) {
        if (buffer.start != nullptr && buffer.start != MAP_FAILED && buffer.length > 0) {
            if (::munmap(buffer.start, buffer.length) < 0) {
                std::cerr << "[WARN] munmap failed: " << std::strerror(errno) << "\n";
            }
        }

        buffer.start = nullptr;
        buffer.length = 0;
    }

    buffers_.clear();
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

void CameraDevice::set_format(__u32 width, __u32 height, const std::string& pixel_format) {
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

    current_width_ = current.fmt.pix.width;
    current_height_ = current.fmt.pix.height;
    current_pixelformat_ = current.fmt.pix.pixelformat;
    current_sizeimage_ = current.fmt.pix.sizeimage;

    std::cout << "---------- Current Format ----------\n";
    std::cout << "current     : "
              << current.fmt.pix.width << "x" << current.fmt.pix.height
              << " " << fourcc_to_string(current.fmt.pix.pixelformat)
              << "\n";
    std::cout << "bytesperline: " << current.fmt.pix.bytesperline << "\n";
    std::cout << "sizeimage   : " << current.fmt.pix.sizeimage << "\n";
    std::cout << "================================\n";
}

void CameraDevice::init_mmap_buffers(__u32 requested_buffer_count) {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (requested_buffer_count == 0 || requested_buffer_count > 32) {
        throw std::runtime_error("Requested buffer count must be in range [1, 32]");
    }

    release_mmap_buffers();

    v4l2_requestbuffers req{};
    req.count = requested_buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    std::cout << "========== Init MMAP Buffers ==========\n";
    std::cout << "requested buffers: " << requested_buffer_count << "\n";

    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        throw std::runtime_error(
            "VIDIOC_REQBUFS failed: " + std::string(std::strerror(errno))
        );
    }

    std::cout << "driver buffers   : " << req.count << "\n";

    if (req.count < 2) {
        throw std::runtime_error("Insufficient buffer memory: driver returned fewer than 2 buffers");
    }

    buffers_.resize(req.count);

    for (__u32 i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            throw std::runtime_error(
                "VIDIOC_QUERYBUF failed at index " + std::to_string(i) +
                ": " + std::string(std::strerror(errno))
            );
        }

        void* start = ::mmap(
            nullptr,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            buf.m.offset
        );

        if (start == MAP_FAILED) {
            throw std::runtime_error(
                "mmap failed at index " + std::to_string(i) +
                ": " + std::string(std::strerror(errno))
            );
        }

        buffers_[i].start = start;
        buffers_[i].length = buf.length;

        std::cout << "buffer[" << i << "]"
                  << " length=" << buf.length
                  << " offset=" << buf.m.offset
                  << " mapped_addr=" << start
                  << "\n";
    }

    for (__u32 i = 0; i < req.count; ++i) {
        requeue_buffer(i);
        std::cout << "queued buffer[" << i << "]\n";
    }

    std::cout << "MMAP buffer initialization done.\n";
    std::cout << "=======================================\n";
}

void CameraDevice::requeue_buffer(__u32 index) {
    if (index >= buffers_.size()) {
        throw std::runtime_error("Cannot queue invalid buffer index: " + std::to_string(index));
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        throw std::runtime_error(
            "VIDIOC_QBUF failed at index " + std::to_string(index) +
            ": " + std::string(std::strerror(errno))
        );
    }
}

bool CameraDevice::try_requeue_buffer_noexcept(__u32 index) noexcept {
    try {
        requeue_buffer(index);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to requeue V4L2 buffer "
                  << index << ": " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[ERROR] Failed to requeue V4L2 buffer "
                  << index << ": unknown error\n";
    }

    return false;
}

void CameraDevice::start_streaming() {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (buffers_.empty()) {
        throw std::runtime_error("Cannot start streaming before MMAP buffers are initialized");
    }

    if (streaming_) {
        return;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (::ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        throw std::runtime_error(
            "VIDIOC_STREAMON failed: " + std::string(std::strerror(errno))
        );
    }

    streaming_ = true;
    std::cout << "[INFO] Streaming started.\n";
}

void CameraDevice::stop_streaming() {
    if (fd_ < 0 || !streaming_) {
        return;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (::ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        std::cerr << "[WARN] VIDIOC_STREAMOFF failed: " << std::strerror(errno) << "\n";
    } else {
        std::cout << "[INFO] Streaming stopped.\n";
    }

    streaming_ = false;
}

void CameraDevice::capture_one_frame(int timeout_ms) {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (!streaming_) {
        throw std::runtime_error("Cannot capture frame before streaming is started");
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    std::cout << "========== Capture One Frame ==========\n";
    std::cout << "poll timeout: " << timeout_ms << " ms\n";

    int poll_ret = 0;
    do {
        poll_ret = ::poll(&pfd, 1, timeout_ms);
    } while (poll_ret < 0 && errno == EINTR);

    if (poll_ret < 0) {
        throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
    }

    if (poll_ret == 0) {
        throw std::runtime_error("poll timed out: no frame received");
    }

    if (pfd.revents & POLLERR) {
        std::cout << "[WARN] poll returned POLLERR\n";
    }

    if (!(pfd.revents & POLLIN)) {
        throw std::runtime_error("poll returned but POLLIN is not set, revents=" + std::to_string(pfd.revents));
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            throw std::runtime_error("VIDIOC_DQBUF returned EAGAIN: no buffer ready");
        }

        throw std::runtime_error(
            "VIDIOC_DQBUF failed: " + std::string(std::strerror(errno))
        );
    }

    if (buf.index >= buffers_.size()) {
        throw std::runtime_error("VIDIOC_DQBUF returned invalid buffer index: " + std::to_string(buf.index));
    }

    std::cout << "dequeued index : " << buf.index << "\n";
    std::cout << "bytesused      : " << buf.bytesused << "\n";
    std::cout << "buffer length  : " << buffers_[buf.index].length << "\n";
    std::cout << "sequence       : " << buf.sequence << "\n";
    std::cout << "buffer flags   : 0x" << std::hex << buf.flags << std::dec
              << " error=" << ((buf.flags & V4L2_BUF_FLAG_ERROR) ? "yes" : "no") << "\n";
    std::cout << "timestamp      : "
              << buf.timestamp.tv_sec << "."
              << std::setw(6) << std::setfill('0') << buf.timestamp.tv_usec
              << std::setfill(' ') << "\n";

    if (buf.bytesused == 0) {
        std::cout << "[WARN] Captured frame has 0 bytes.\n";
    } else if (buf.bytesused > buffers_[buf.index].length) {
        std::cout << "[WARN] bytesused is larger than mapped buffer length.\n";
    } else {
        const auto* data = static_cast<const unsigned char*>(buffers_[buf.index].start);
        const size_t preview_count = std::min<size_t>(16, buf.bytesused);

        std::cout << "first bytes    :";
        for (size_t i = 0; i < preview_count; ++i) {
            std::cout << " "
                      << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(data[i])
                      << std::dec << std::setfill(' ');
        }
        std::cout << "\n";
    }

    requeue_buffer(buf.index);
    std::cout << "requeued index : " << buf.index << "\n";
    std::cout << "=======================================\n";
}


CameraDevice::CapturedFrameInfo CameraDevice::dequeue_frame(int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int poll_ret = 0;
    do {
        poll_ret = ::poll(&pfd, 1, timeout_ms);
    } while (poll_ret < 0 && errno == EINTR);

    if (poll_ret < 0) {
        const int error_code = errno;
        const CameraCaptureErrorKind kind =
            (error_code == ENODEV || error_code == ENXIO || error_code == EBADF)
                ? CameraCaptureErrorKind::DeviceUnavailable
                : CameraCaptureErrorKind::StreamFailure;
        throw CameraCaptureError(
            kind,
            error_code,
            "poll failed: " + std::string(std::strerror(error_code))
        );
    }

    if (poll_ret == 0) {
        throw CameraCaptureError(
            CameraCaptureErrorKind::Timeout,
            0,
            "poll timed out: no frame received"
        );
    }

    if (pfd.revents & (POLLNVAL | POLLHUP)) {
        throw CameraCaptureError(
            CameraCaptureErrorKind::DeviceUnavailable,
            0,
            "poll reported device unavailable, revents=" +
                std::to_string(pfd.revents)
        );
    }

    if ((pfd.revents & POLLERR) && !(pfd.revents & POLLIN)) {
        throw CameraCaptureError(
            CameraCaptureErrorKind::StreamFailure,
            0,
            "poll reported stream error without readable data, revents=" +
                std::to_string(pfd.revents)
        );
    }

    if (!(pfd.revents & POLLIN)) {
        throw CameraCaptureError(
            CameraCaptureErrorKind::StreamFailure,
            0,
            "poll returned but POLLIN is not set, revents=" +
                std::to_string(pfd.revents)
        );
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        const int error_code = errno;
        if (error_code == EAGAIN) {
            throw CameraCaptureError(
                CameraCaptureErrorKind::TemporaryUnavailable,
                error_code,
                "VIDIOC_DQBUF returned EAGAIN: no buffer ready"
            );
        }

        const CameraCaptureErrorKind kind =
            (error_code == ENODEV || error_code == ENXIO || error_code == EBADF)
                ? CameraCaptureErrorKind::DeviceUnavailable
                : CameraCaptureErrorKind::StreamFailure;
        throw CameraCaptureError(
            kind,
            error_code,
            "VIDIOC_DQBUF failed: " + std::string(std::strerror(error_code))
        );
    }

    if (buf.index >= buffers_.size()) {
        throw CameraCaptureError(
            CameraCaptureErrorKind::InvalidBufferMetadata,
            0,
            "VIDIOC_DQBUF returned invalid buffer index: " +
                std::to_string(buf.index)
        );
    }

    if (buf.bytesused == 0) {
        std::cout << "[WARN] Captured frame has 0 bytes.\n";
    }

    if (buf.bytesused > buffers_[buf.index].length) {
        try_requeue_buffer_noexcept(buf.index);
        throw CameraCaptureError(
            CameraCaptureErrorKind::InvalidBufferMetadata,
            0,
            "bytesused is larger than mapped buffer length"
        );
    }

    CapturedFrameInfo info{};
    info.index = buf.index;
    info.bytesused = buf.bytesused;
    info.sequence = buf.sequence;
    info.flags = buf.flags;
    info.timestamp = buf.timestamp;
    return info;
}

unsigned char CameraDevice::clamp_to_u8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<unsigned char>(value);
}

void CameraDevice::yuyv_to_rgb(
    const unsigned char* yuyv,
    std::vector<unsigned char>& rgb,
    __u32 width,
    __u32 height
) {
    rgb.resize(static_cast<size_t>(width) * height * 3);

    size_t in = 0;
    size_t out = 0;
    const size_t pixel_count = static_cast<size_t>(width) * height;

    for (size_t i = 0; i + 1 < pixel_count; i += 2) {
        int y0 = yuyv[in + 0];
        int u  = yuyv[in + 1];
        int y1 = yuyv[in + 2];
        int v  = yuyv[in + 3];
        in += 4;

        auto convert = [](int y, int u_val, int v_val) {
            int c = y - 16;
            int d = u_val - 128;
            int e = v_val - 128;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            return std::array<unsigned char, 3>{
                CameraDevice::clamp_to_u8(r),
                CameraDevice::clamp_to_u8(g),
                CameraDevice::clamp_to_u8(b)
            };
        };

        auto rgb0 = convert(y0, u, v);
        auto rgb1 = convert(y1, u, v);

        rgb[out++] = rgb0[0];
        rgb[out++] = rgb0[1];
        rgb[out++] = rgb0[2];

        rgb[out++] = rgb1[0];
        rgb[out++] = rgb1[1];
        rgb[out++] = rgb1[2];
    }
}

void CameraDevice::save_current_frame_to_files(
    const CapturedFrameInfo& frame,
    const std::string& output_dir
) const {
    if (frame.index >= buffers_.size()) {
        throw std::runtime_error("Invalid frame buffer index when saving");
    }

    if (current_width_ == 0 || current_height_ == 0 || current_pixelformat_ == 0) {
        throw std::runtime_error("Current format is unknown, set format before saving");
    }

    std::filesystem::create_directories(output_dir);

    std::ostringstream base;
    base << output_dir << "/frame_"
         << std::setw(6) << std::setfill('0') << frame.sequence;

    const auto* data = static_cast<const unsigned char*>(buffers_[frame.index].start);

    const std::string raw_path = base.str() + "." + fourcc_to_string(current_pixelformat_);
    {
        std::ofstream raw(raw_path, std::ios::binary);
        if (!raw) {
            throw std::runtime_error("Failed to open raw output file: " + raw_path);
        }

        raw.write(reinterpret_cast<const char*>(data), frame.bytesused);
        if (!raw) {
            throw std::runtime_error("Failed to write raw output file: " + raw_path);
        }
    }

    std::cout << "[INFO] Saved raw frame: " << raw_path
              << " bytes=" << frame.bytesused << "\n";

    if (current_pixelformat_ == string_to_fourcc("YUYV")) {
        std::vector<unsigned char> rgb;
        yuyv_to_rgb(data, rgb, current_width_, current_height_);

        const std::string ppm_path = base.str() + ".ppm";
        std::ofstream ppm(ppm_path, std::ios::binary);
        if (!ppm) {
            throw std::runtime_error("Failed to open ppm output file: " + ppm_path);
        }

        ppm << "P6\n" << current_width_ << " " << current_height_ << "\n255\n";
        ppm.write(
            reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size())
        );

        if (!ppm) {
            throw std::runtime_error("Failed to write ppm output file: " + ppm_path);
        }

        std::cout << "[INFO] Saved PPM frame: " << ppm_path
                  << " bytes=" << rgb.size() << "\n";
    } else {
        std::cout << "[WARN] PPM conversion is only implemented for YUYV.\n";
    }
}

void CameraDevice::capture_one_frame_to_files(int timeout_ms, const std::string& output_dir) {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (!streaming_) {
        throw std::runtime_error("Cannot capture frame before streaming is started");
    }

    std::cout << "========== Capture One Frame To Files ==========\n";
    std::cout << "poll timeout: " << timeout_ms << " ms\n";

    CapturedFrameInfo frame = dequeue_frame(timeout_ms);
    DequeuedBufferGuard buffer_guard(*this, frame.index);

    std::cout << "dequeued index : " << frame.index << "\n";
    std::cout << "bytesused      : " << frame.bytesused << "\n";
    std::cout << "buffer length  : " << buffers_[frame.index].length << "\n";
    std::cout << "sequence       : " << frame.sequence << "\n";
    std::cout << "buffer flags   : 0x" << std::hex << frame.flags << std::dec
              << " error=" << ((frame.flags & V4L2_BUF_FLAG_ERROR) ? "yes" : "no") << "\n";
    std::cout << "timestamp      : "
              << frame.timestamp.tv_sec << "."
              << std::setw(6) << std::setfill('0') << frame.timestamp.tv_usec
              << std::setfill(' ') << "\n";

    save_current_frame_to_files(frame, output_dir);

    buffer_guard.requeue_or_throw();
    std::cout << "requeued index : " << frame.index << "\n";
    std::cout << "===============================================\n";
}


Frame CameraDevice::capture_frame_copy(int timeout_ms) {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (!streaming_) {
        throw std::runtime_error("Cannot capture frame before streaming is started");
    }

    if (current_width_ == 0 || current_height_ == 0 || current_pixelformat_ == 0) {
        throw std::runtime_error("Current format is unknown, set format before capturing frame copy");
    }

    CapturedFrameInfo info = dequeue_frame(timeout_ms);
    DequeuedBufferGuard buffer_guard(*this, info.index);
    const auto host_receive_time = std::chrono::steady_clock::now();
    const auto capture_system_time = std::chrono::system_clock::now().time_since_epoch();
    const auto capture_timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(capture_system_time).count()
    );

    const auto* data = static_cast<const std::uint8_t*>(buffers_[info.index].start);

    Frame frame;
    frame.data.assign(data, data + info.bytesused);
    frame.width = current_width_;
    frame.height = current_height_;
    frame.pixel_format = current_pixelformat_;
    frame.bytesused = info.bytesused;
    frame.sequence = info.sequence;
    frame.buffer_index = info.index;
    frame.v4l2_flags = info.flags;
    frame.v4l2_timestamp = info.timestamp;
    frame.capture_timestamp_ns = capture_timestamp_ns;
    frame.host_receive_time = host_receive_time;

    buffer_guard.requeue_or_throw();
    return frame;
}

void CameraDevice::capture_frames(int frame_count, int timeout_ms) {
    if (fd_ < 0) {
        throw std::runtime_error("Device is not opened");
    }

    if (!streaming_) {
        throw std::runtime_error("Cannot capture frames before streaming is started");
    }

    if (frame_count <= 0) {
        throw std::runtime_error("Frame count must be positive");
    }

    std::cout << "========== Capture Frames ==========\n";
    std::cout << "target frames : " << frame_count << "\n";
    std::cout << "poll timeout  : " << timeout_ms << " ms\n";

    using Clock = std::chrono::steady_clock;

    std::vector<double> intervals_ms;
    intervals_ms.reserve(static_cast<size_t>(frame_count > 1 ? frame_count - 1 : 0));

    int captured = 0;
    int poll_timeouts = 0;
    int dqbuf_errors = 0;

    std::uint64_t total_bytes = 0;
    __u32 min_bytes = 0;
    __u32 max_bytes = 0;

    bool have_previous_time = false;
    Clock::time_point previous_time{};
    const auto start_time = Clock::now();

    while (captured < frame_count) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int poll_ret = 0;
        do {
            poll_ret = ::poll(&pfd, 1, timeout_ms);
        } while (poll_ret < 0 && errno == EINTR);

        if (poll_ret < 0) {
            throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
        }

        if (poll_ret == 0) {
            ++poll_timeouts;
            continue;
        }

        if (pfd.revents & POLLERR) {
            std::cout << "[WARN] poll returned POLLERR\n";
        }

        if (pfd.revents & POLLHUP) {
            std::cout << "[WARN] poll returned POLLHUP\n";
        }

        if (pfd.revents & POLLNVAL) {
            throw std::runtime_error("poll returned POLLNVAL: invalid file descriptor");
        }

        if (!(pfd.revents & POLLIN)) {
            ++dqbuf_errors;
            std::cout << "[WARN] poll returned without POLLIN, revents="
                      << pfd.revents << "\n";
            continue;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            ++dqbuf_errors;

            if (errno == EAGAIN) {
                continue;
            }

            throw std::runtime_error(
                "VIDIOC_DQBUF failed: " + std::string(std::strerror(errno))
            );
        }

        if (buf.index >= buffers_.size()) {
            throw std::runtime_error(
                "VIDIOC_DQBUF returned invalid buffer index: " + std::to_string(buf.index)
            );
        }

        if (buf.bytesused > buffers_[buf.index].length) {
            requeue_buffer(buf.index);
            throw std::runtime_error("bytesused is larger than mapped buffer length");
        }

        const auto now = Clock::now();

        if (have_previous_time) {
            const double interval_ms =
                std::chrono::duration<double, std::milli>(now - previous_time).count();
            intervals_ms.push_back(interval_ms);
        }

        previous_time = now;
        have_previous_time = true;

        total_bytes += buf.bytesused;

        if (captured == 0) {
            min_bytes = buf.bytesused;
            max_bytes = buf.bytesused;
        } else {
            min_bytes = std::min(min_bytes, buf.bytesused);
            max_bytes = std::max(max_bytes, buf.bytesused);
        }

        ++captured;

        if (captured == 1 || captured == frame_count || captured % 50 == 0) {
            std::cout << "[INFO] captured "
                      << captured << "/" << frame_count
                      << " sequence=" << buf.sequence
                      << " bytesused=" << buf.bytesused
                      << "\n";
        }

        requeue_buffer(buf.index);
    }

    const auto end_time = Clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(end_time - start_time).count();

    double avg_interval_ms = 0.0;
    double max_interval_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;

    if (!intervals_ms.empty()) {
        double sum = 0.0;
        for (double v : intervals_ms) {
            sum += v;
            if (v > max_interval_ms) {
                max_interval_ms = v;
            }
        }

        avg_interval_ms = sum / static_cast<double>(intervals_ms.size());

        std::vector<double> sorted = intervals_ms;
        std::sort(sorted.begin(), sorted.end());

        auto percentile = [&sorted](double p) {
            if (sorted.empty()) {
                return 0.0;
            }

            const double rank =
                (p / 100.0) * static_cast<double>(sorted.size() - 1);
            const size_t index = static_cast<size_t>(rank);
            return sorted[index];
        };

        p50_ms = percentile(50.0);
        p95_ms = percentile(95.0);
        p99_ms = percentile(99.0);
    }

    const double fps = elapsed_s > 0.0
        ? static_cast<double>(captured) / elapsed_s
        : 0.0;

    const double avg_bytes = captured > 0
        ? static_cast<double>(total_bytes) / static_cast<double>(captured)
        : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "========== Capture Statistics ==========\n";
    std::cout << "requested frames      : " << frame_count << "\n";
    std::cout << "captured frames       : " << captured << "\n";
    std::cout << "elapsed seconds       : " << elapsed_s << "\n";
    std::cout << "actual FPS            : " << fps << "\n";
    std::cout << "interval samples      : " << intervals_ms.size() << "\n";
    std::cout << "avg interval ms       : " << avg_interval_ms << "\n";
    std::cout << "p50 interval ms       : " << p50_ms << "\n";
    std::cout << "p95 interval ms       : " << p95_ms << "\n";
    std::cout << "p99 interval ms       : " << p99_ms << "\n";
    std::cout << "max interval ms       : " << max_interval_ms << "\n";
    std::cout << "poll timeouts         : " << poll_timeouts << "\n";
    std::cout << "dqbuf errors          : " << dqbuf_errors << "\n";
    std::cout << "min bytesused         : " << min_bytes << "\n";
    std::cout << "max bytesused         : " << max_bytes << "\n";
    std::cout << "avg bytesused         : " << avg_bytes << "\n";
    std::cout << "========================================\n";
    std::cout.unsetf(std::ios::floatfield);
}

