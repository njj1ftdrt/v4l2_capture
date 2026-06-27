#pragma once

#include <linux/videodev2.h>

#include <string>

class CameraDevice {
public:
    explicit CameraDevice(std::string device_path);
    ~CameraDevice();

    CameraDevice(const CameraDevice&) = delete;
    CameraDevice& operator=(const CameraDevice&) = delete;

    CameraDevice(CameraDevice&& other) noexcept;
    CameraDevice& operator=(CameraDevice&& other) noexcept;

    void open_device();
    void query_capability() const;

private:
    std::string device_path_;
    int fd_{-1};

    static void print_capability_flag(__u32 caps, __u32 flag, const std::string& name);
};
