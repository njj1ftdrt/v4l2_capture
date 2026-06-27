#pragma once

#include <linux/videodev2.h>

#include <cstddef>
#include <string>
#include <vector>

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
    void set_format(__u32 width, __u32 height, const std::string& pixel_format);
    void init_mmap_buffers(__u32 requested_buffer_count);

    void start_streaming();
    void stop_streaming();
    void capture_one_frame(int timeout_ms);
    void capture_one_frame_to_files(int timeout_ms, const std::string& output_dir);

private:
    struct MappedBuffer {
        void* start{nullptr};
        size_t length{0};
    };

    struct CapturedFrameInfo {
        __u32 index{0};
        __u32 bytesused{0};
        __u32 sequence{0};
        timeval timestamp{};
    };

    std::string device_path_;
    int fd_{-1};
    std::vector<MappedBuffer> buffers_;
    bool streaming_{false};

    __u32 current_width_{0};
    __u32 current_height_{0};
    __u32 current_pixelformat_{0};
    __u32 current_sizeimage_{0};

    void close_device();
    void release_mmap_buffers();
    void requeue_buffer(__u32 index);
    CapturedFrameInfo dequeue_frame(int timeout_ms);

    void save_current_frame_to_files(
        const CapturedFrameInfo& frame,
        const std::string& output_dir
    ) const;

    static void print_capability_flag(__u32 caps, __u32 flag, const std::string& name);
    static std::string fourcc_to_string(__u32 pixelformat);
    static __u32 string_to_fourcc(const std::string& fourcc);

    static unsigned char clamp_to_u8(int value);
    static void yuyv_to_rgb(
        const unsigned char* yuyv,
        std::vector<unsigned char>& rgb,
        __u32 width,
        __u32 height
    );

    void list_frame_sizes(__u32 pixelformat) const;
    void list_frame_intervals(__u32 pixelformat, __u32 width, __u32 height) const;
};
