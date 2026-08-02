#include "app_config.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string trim(const std::string& input) {
    auto begin = input.begin();
    while (begin != input.end() && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }

    auto end = input.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }

    return std::string(begin, end);
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int parse_positive_int(const std::string& value, const std::string& name, int max_value = 1000000) {
    try {
        size_t pos = 0;
        const long parsed = std::stol(value, &pos, 10);
        if (pos != value.size() || parsed <= 0 || parsed > max_value) {
            throw std::runtime_error("out of valid range");
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid integer for " + name + ": " + value);
    }
}

int parse_non_negative_int(const std::string& value, const std::string& name, int max_value = 1000000) {
    try {
        size_t pos = 0;
        const long parsed = std::stol(value, &pos, 10);
        if (pos != value.size() || parsed < 0 || parsed > max_value) {
            throw std::runtime_error("out of valid range");
        }
        return static_cast<int>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid integer for " + name + ": " + value);
    }
}

__u32 parse_u32_value(const std::string& value, const std::string& name) {
    const int parsed = parse_positive_int(value, name, 1000000);
    return static_cast<__u32>(parsed);
}

bool parse_bool(const std::string& value, const std::string& name) {
    const std::string lowered = to_lower(trim(value));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    throw std::runtime_error("Invalid bool for " + name + ": " + value);
}

void apply_key_value(AppConfig& config, const std::string& key, const std::string& value) {
    if (key == "device") {
        config.device = value;
    } else if (key == "list_formats") {
        config.list_formats = parse_bool(value, key);
    } else if (key == "width") {
        config.width = parse_u32_value(value, key);
    } else if (key == "height") {
        config.height = parse_u32_value(value, key);
    } else if (key == "format" || key == "pixel_format") {
        config.pixel_format = value;
    } else if (key == "mmap_buffers") {
        config.mmap_buffers = parse_u32_value(value, key);
    } else if (key == "capture_one") {
        config.capture_one = parse_bool(value, key);
    } else if (key == "save_one") {
        config.save_one = parse_bool(value, key);
    } else if (key == "capture_frames") {
        config.capture_frames = parse_non_negative_int(value, key);
    } else if (key == "frames" || key == "pipeline_frames") {
        config.pipeline_frames = parse_non_negative_int(value, key);
    } else if (key == "ring_capacity") {
        config.ring_capacity = parse_positive_int(value, key);
    } else if (key == "consumer_delay_ms") {
        config.consumer_delay_ms = parse_non_negative_int(value, key);
    } else if (key == "pipeline_save") {
        config.pipeline_save = parse_bool(value, key);
    } else if (key == "save_limit") {
        config.save_limit = parse_positive_int(value, key);
    } else if (key == "output_dir") {
        config.output_dir = value;
    } else if (key == "stats_output") {
        config.stats_output = value;
    } else if (key == "tcp_host") {
        if (value.empty()) {
            config.tcp_host.reset();
        } else {
            config.tcp_host = value;
        }
    } else if (key == "tcp_port") {
        config.tcp_port = parse_non_negative_int(value, key, 65535);
    } else if (key == "tcp_queue_capacity") {
        config.tcp_queue_capacity = parse_positive_int(value, key);
    } else if (key == "tcp_connect_max_attempts") {
        config.tcp_connect_max_attempts = parse_positive_int(value, key, 1000);
    } else if (key == "tcp_connect_retry_delay_ms") {
        config.tcp_connect_retry_delay_ms = parse_non_negative_int(value, key, 60000);
    } else if (key == "tcp_send_timeout_ms") {
        config.tcp_send_timeout_ms = parse_positive_int(value, key, 600000);
    } else if (key == "timeout_ms") {
        config.timeout_ms = parse_positive_int(value, key);
    } else if (key == "camera_recovery_enabled") {
        config.camera_recovery_enabled = parse_bool(value, key);
    } else if (key == "camera_timeout_recovery_threshold") {
        config.camera_timeout_recovery_threshold = parse_positive_int(value, key, 1000);
    } else if (key == "camera_invalid_frame_recovery_threshold") {
        config.camera_invalid_frame_recovery_threshold = parse_positive_int(value, key, 1000);
    } else if (key == "camera_invalid_frame_recovery_cooldown_frames") {
        config.camera_invalid_frame_recovery_cooldown_frames = parse_non_negative_int(value, key, 1000000);
    } else if (key == "camera_invalid_frame_recovery_max_count") {
        config.camera_invalid_frame_recovery_max_count = parse_positive_int(value, key, 1000);
    } else if (key == "camera_recovery_max_attempts") {
        config.camera_recovery_max_attempts = parse_positive_int(value, key, 1000);
    } else if (key == "camera_recovery_retry_delay_ms") {
        config.camera_recovery_retry_delay_ms = parse_non_negative_int(value, key, 600000);
    } else if (key == "log_level") {
        config.log_level = value;
    } else {
        throw std::runtime_error("Unknown config key: " + key);
    }
}

void load_config_file_into(AppConfig& config, const std::string& path, bool required) {
    std::ifstream file(path);
    if (!file) {
        if (required) {
            throw std::runtime_error("Config file not found: " + path);
        }
        return;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        const std::size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            throw std::runtime_error(
                "Invalid config line " + std::to_string(line_no) +
                " in " + path + ": missing '='"
            );
        }

        const std::string key = trim(line.substr(0, eq_pos));
        const std::string value = trim(line.substr(eq_pos + 1));
        if (key.empty()) {
            throw std::runtime_error(
                "Invalid config line " + std::to_string(line_no) +
                " in " + path + ": empty key"
            );
        }

        try {
            apply_key_value(config, key, value);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Config error at " + path + ":" + std::to_string(line_no) +
                ": " + e.what()
            );
        }
    }
}

std::optional<std::string> find_config_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--config requires a file path");
            }
            return std::string(argv[i + 1]);
        }
    }
    return std::nullopt;
}

void apply_cli_args(AppConfig& config, int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            config.config_path = argv[++i];
        } else if ((arg == "--device" || arg == "-d") && i + 1 < argc) {
            config.device = argv[++i];
        } else if (arg == "--list-formats") {
            config.list_formats = true;
        } else if (arg == "--width" && i + 1 < argc) {
            config.width = parse_u32_value(argv[++i], "--width");
        } else if (arg == "--height" && i + 1 < argc) {
            config.height = parse_u32_value(argv[++i], "--height");
        } else if (arg == "--format" && i + 1 < argc) {
            config.pixel_format = argv[++i];
        } else if (arg == "--mmap-buffers" && i + 1 < argc) {
            config.mmap_buffers = parse_u32_value(argv[++i], "--mmap-buffers");
        } else if (arg == "--capture-one") {
            config.capture_one = true;
        } else if (arg == "--save-one") {
            config.save_one = true;
        } else if ((arg == "--frames" || arg == "--capture-frames") && i + 1 < argc) {
            config.capture_frames = parse_positive_int(argv[++i], arg);
        } else if (arg == "--pipeline-frames" && i + 1 < argc) {
            config.pipeline_frames = parse_positive_int(argv[++i], arg);
        } else if (arg == "--ring-capacity" && i + 1 < argc) {
            config.ring_capacity = parse_positive_int(argv[++i], arg);
        } else if (arg == "--consumer-delay-ms" && i + 1 < argc) {
            config.consumer_delay_ms = parse_non_negative_int(argv[++i], arg);
        } else if (arg == "--pipeline-save") {
            config.pipeline_save = true;
        } else if (arg == "--save-limit" && i + 1 < argc) {
            config.save_limit = parse_positive_int(argv[++i], arg);
        } else if (arg == "--output" && i + 1 < argc) {
            config.output_dir = argv[++i];
        } else if (arg == "--stats-output" && i + 1 < argc) {
            config.stats_output = argv[++i];
        } else if (arg == "--tcp-host" && i + 1 < argc) {
            config.tcp_host = argv[++i];
        } else if (arg == "--tcp-port" && i + 1 < argc) {
            config.tcp_port = parse_positive_int(argv[++i], arg, 65535);
        } else if (arg == "--tcp-queue-capacity" && i + 1 < argc) {
            config.tcp_queue_capacity = parse_positive_int(argv[++i], arg);
        } else if (arg == "--tcp-connect-max-attempts" && i + 1 < argc) {
            config.tcp_connect_max_attempts = parse_positive_int(argv[++i], arg, 1000);
        } else if (arg == "--tcp-connect-retry-delay-ms" && i + 1 < argc) {
            config.tcp_connect_retry_delay_ms = parse_non_negative_int(argv[++i], arg, 60000);
        } else if (arg == "--tcp-send-timeout-ms" && i + 1 < argc) {
            config.tcp_send_timeout_ms = parse_positive_int(argv[++i], arg, 600000);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            config.timeout_ms = parse_positive_int(argv[++i], arg);
        } else if (arg == "--camera-recovery") {
            config.camera_recovery_enabled = true;
        } else if (arg == "--no-camera-recovery") {
            config.camera_recovery_enabled = false;
        } else if (arg == "--camera-timeout-recovery-threshold" && i + 1 < argc) {
            config.camera_timeout_recovery_threshold = parse_positive_int(argv[++i], arg, 1000);
        } else if (arg == "--camera-invalid-frame-recovery-threshold" && i + 1 < argc) {
            config.camera_invalid_frame_recovery_threshold = parse_positive_int(argv[++i], arg, 1000);
        } else if (arg == "--camera-invalid-frame-recovery-cooldown-frames" && i + 1 < argc) {
            config.camera_invalid_frame_recovery_cooldown_frames = parse_non_negative_int(argv[++i], arg, 1000000);
        } else if (arg == "--camera-invalid-frame-recovery-max-count" && i + 1 < argc) {
            config.camera_invalid_frame_recovery_max_count = parse_positive_int(argv[++i], arg, 1000);
        } else if (arg == "--camera-recovery-max-attempts" && i + 1 < argc) {
            config.camera_recovery_max_attempts = parse_positive_int(argv[++i], arg, 1000);
        } else if (arg == "--camera-recovery-retry-delay-ms" && i + 1 < argc) {
            config.camera_recovery_retry_delay_ms = parse_non_negative_int(argv[++i], arg, 600000);
        } else if (arg == "--log-level" && i + 1 < argc) {
            config.log_level = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            // Handled by main before load_app_config is called.
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }
}

std::string optional_u32_to_string(const std::optional<__u32>& value) {
    return value ? std::to_string(*value) : "unset";
}

std::string optional_string_to_string(const std::optional<std::string>& value) {
    return value ? *value : "unset";
}

}  // namespace

AppConfig load_app_config(int argc, char* argv[]) {
    AppConfig config;

    const auto explicit_config_path = find_config_arg(argc, argv);
    if (explicit_config_path.has_value()) {
        config.config_path = *explicit_config_path;
        load_config_file_into(config, config.config_path, true);
    } else {
        load_config_file_into(config, config.config_path, false);
    }

    apply_cli_args(config, argc, argv);
    validate_app_config(config);
    return config;
}

void validate_app_config(const AppConfig& config) {
    const bool wants_set_format =
        config.width.has_value() || config.height.has_value() || config.pixel_format.has_value();

    if (wants_set_format &&
        !(config.width.has_value() && config.height.has_value() && config.pixel_format.has_value())) {
        throw std::runtime_error("Setting format requires width, height and format together");
    }

    if (config.mmap_buffers.has_value() && !wants_set_format) {
        throw std::runtime_error("Initializing MMAP buffers requires setting format first");
    }

    const int capture_mode_count =
        (config.capture_one ? 1 : 0) +
        (config.save_one ? 1 : 0) +
        (config.capture_frames > 0 ? 1 : 0) +
        (config.pipeline_frames > 0 ? 1 : 0);

    if (capture_mode_count > 1) {
        throw std::runtime_error(
            "Use only one capture mode: capture_one, save_one, capture_frames, or pipeline_frames"
        );
    }

    const bool tcp_enabled = config.tcp_host.has_value() || config.tcp_port > 0;
    if (tcp_enabled && !(config.tcp_host.has_value() && config.tcp_port > 0)) {
        throw std::runtime_error("TCP sending requires tcp_host and tcp_port together");
    }

    if (tcp_enabled && config.pipeline_frames <= 0) {
        throw std::runtime_error("TCP sending is currently supported only with pipeline_frames");
    }

    if (capture_mode_count > 0 && !config.mmap_buffers.has_value()) {
        throw std::runtime_error("Capturing frames requires mmap_buffers");
    }

    if (config.device.empty()) {
        throw std::runtime_error("device must not be empty");
    }

    if (config.output_dir.empty()) {
        throw std::runtime_error("output_dir must not be empty");
    }

    if (config.stats_output.empty()) {
        throw std::runtime_error("stats_output must not be empty");
    }

    if (config.tcp_port < 0 || config.tcp_port > 65535) {
        throw std::runtime_error("tcp_port must be between 0 and 65535");
    }

    if (config.ring_capacity <= 0 || config.tcp_queue_capacity <= 0) {
        throw std::runtime_error("ring capacities must be positive");
    }

    if (config.tcp_connect_max_attempts <= 0) {
        throw std::runtime_error("tcp_connect_max_attempts must be positive");
    }

    if (config.tcp_connect_retry_delay_ms < 0) {
        throw std::runtime_error("tcp_connect_retry_delay_ms must be non-negative");
    }

    if (config.tcp_send_timeout_ms <= 0) {
        throw std::runtime_error("tcp_send_timeout_ms must be positive");
    }

    if (config.camera_timeout_recovery_threshold <= 0) {
        throw std::runtime_error("camera_timeout_recovery_threshold must be positive");
    }

    if (config.camera_invalid_frame_recovery_threshold <= 0) {
        throw std::runtime_error("camera_invalid_frame_recovery_threshold must be positive");
    }

    if (config.camera_invalid_frame_recovery_cooldown_frames < 0) {
        throw std::runtime_error("camera_invalid_frame_recovery_cooldown_frames must be non-negative");
    }

    if (config.camera_invalid_frame_recovery_max_count <= 0) {
        throw std::runtime_error("camera_invalid_frame_recovery_max_count must be positive");
    }

    if (config.camera_recovery_max_attempts <= 0) {
        throw std::runtime_error("camera_recovery_max_attempts must be positive");
    }

    if (config.camera_recovery_retry_delay_ms < 0) {
        throw std::runtime_error("camera_recovery_retry_delay_ms must be non-negative");
    }

    const std::set<std::string> allowed_log_levels{"DEBUG", "INFO", "WARN", "ERROR"};
    if (allowed_log_levels.count(config.log_level) == 0) {
        throw std::runtime_error("log_level must be one of DEBUG, INFO, WARN, ERROR");
    }
}

void print_app_config(const AppConfig& config) {
    log_info("CONFIG", "========== Effective AppConfig ==========");
    log_info("CONFIG", "config path        : ", config.config_path);
    log_info("CONFIG", "device             : ", config.device);
    log_info("CONFIG", "list formats       : ", (config.list_formats ? "yes" : "no"));
    log_info("CONFIG", "width              : ", optional_u32_to_string(config.width));
    log_info("CONFIG", "height             : ", optional_u32_to_string(config.height));
    log_info("CONFIG", "format             : ", optional_string_to_string(config.pixel_format));
    log_info("CONFIG", "mmap buffers       : ", optional_u32_to_string(config.mmap_buffers));
    log_info("CONFIG", "capture one        : ", (config.capture_one ? "yes" : "no"));
    log_info("CONFIG", "save one           : ", (config.save_one ? "yes" : "no"));
    log_info("CONFIG", "capture frames     : ", config.capture_frames);
    log_info("CONFIG", "pipeline frames    : ", config.pipeline_frames);
    log_info("CONFIG", "ring capacity      : ", config.ring_capacity);
    log_info("CONFIG", "tcp queue capacity : ", config.tcp_queue_capacity);
    log_info("CONFIG", "tcp connect attempts: ", config.tcp_connect_max_attempts);
    log_info("CONFIG", "tcp retry delay ms  : ", config.tcp_connect_retry_delay_ms);
    log_info("CONFIG", "tcp send timeout ms : ", config.tcp_send_timeout_ms);
    log_info("CONFIG", "camera recovery    : ", (config.camera_recovery_enabled ? "enabled" : "disabled"));
    log_info("CONFIG", "camera timeout threshold: ", config.camera_timeout_recovery_threshold);
    log_info("CONFIG", "camera invalid threshold: ", config.camera_invalid_frame_recovery_threshold);
    log_info("CONFIG", "camera invalid cooldown frames: ", config.camera_invalid_frame_recovery_cooldown_frames);
    log_info("CONFIG", "camera invalid recovery budget: ", config.camera_invalid_frame_recovery_max_count);
    log_info("CONFIG", "camera recovery attempts: ", config.camera_recovery_max_attempts);
    log_info("CONFIG", "camera recovery delay ms: ", config.camera_recovery_retry_delay_ms);
    log_info("CONFIG", "consumer delay ms  : ", config.consumer_delay_ms);
    log_info("CONFIG", "pipeline save      : ", (config.pipeline_save ? "yes" : "no"));
    log_info("CONFIG", "save limit         : ", config.save_limit);
    log_info("CONFIG", "output dir         : ", config.output_dir);
    log_info("CONFIG", "stats output       : ", config.stats_output);
    log_info("CONFIG", "tcp host           : ", optional_string_to_string(config.tcp_host));
    log_info("CONFIG", "tcp port           : ", config.tcp_port);
    log_info("CONFIG", "timeout ms         : ", config.timeout_ms);
    log_info("CONFIG", "log level          : ", config.log_level);
    log_info("CONFIG", "=========================================");
}
