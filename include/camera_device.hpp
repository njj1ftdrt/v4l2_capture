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
    void list_formats() const;
    void set_format(__u32 width, __u32 height, const std::string& pixel_format) const;

private:
    std::string device_path_;
    int fd_{-1};

    static void print_capability_flag(__u32 caps, __u32 flag, const std::string& name);
    static std::string fourcc_to_string(__u32 pixelformat);
    static __u32 string_to_fourcc(const std::string& fourcc);

    void list_frame_sizes(__u32 pixelformat) const;
    void list_frame_intervals(__u32 pixelformat, __u32 width, __u32 height) const;
};
