#pragma once

#include <linux/videodev2.h>

#include <optional>
#include <string>

struct AppConfig {
    std::string config_path = "config/v4l2_tcp_pipeline.conf";

    std::string device = "/dev/video10";
    bool list_formats = false;
    bool capture_one = false;
    bool save_one = false;
    int capture_frames = 0;
    int pipeline_frames = 0;
    int ring_capacity = 8;
    int consumer_delay_ms = 0;
    bool pipeline_save = false;
    int save_limit = 5;
    int timeout_ms = 2000;
    std::string output_dir = "output";

    std::optional<std::string> tcp_host;
    int tcp_port = 0;
    int tcp_queue_capacity = 8;

    std::optional<__u32> width;
    std::optional<__u32> height;
    std::optional<std::string> pixel_format;
    std::optional<__u32> mmap_buffers;

    std::string log_level = "INFO";
};

AppConfig load_app_config(int argc, char* argv[]);
void validate_app_config(const AppConfig& config);
void print_app_config(const AppConfig& config);
