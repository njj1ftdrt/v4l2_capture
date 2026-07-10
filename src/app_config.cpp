#include "app_config.hpp"

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
    } else if (key == "timeout_ms") {
        config.timeout_ms = parse_positive_int(value, key);
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
        } else if (arg == "--tcp-host" && i + 1 < argc) {
            config.tcp_host = argv[++i];
        } else if (arg == "--tcp-port" && i + 1 < argc) {
            config.tcp_port = parse_positive_int(argv[++i], arg, 65535);
        } else if (arg == "--tcp-queue-capacity" && i + 1 < argc) {
            config.tcp_queue_capacity = parse_positive_int(argv[++i], arg);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            config.timeout_ms = parse_positive_int(argv[++i], arg);
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

    if (config.tcp_port < 0 || config.tcp_port > 65535) {
        throw std::runtime_error("tcp_port must be between 0 and 65535");
    }

    if (config.ring_capacity <= 0 || config.tcp_queue_capacity <= 0) {
        throw std::runtime_error("ring capacities must be positive");
    }

    const std::set<std::string> allowed_log_levels{"DEBUG", "INFO", "WARN", "ERROR"};
    if (allowed_log_levels.count(config.log_level) == 0) {
        throw std::runtime_error("log_level must be one of DEBUG, INFO, WARN, ERROR");
    }
}

void print_app_config(const AppConfig& config) {
    std::cout << "========== Effective AppConfig ==========" << "\n";
    std::cout << "config path        : " << config.config_path << "\n";
    std::cout << "device             : " << config.device << "\n";
    std::cout << "list formats       : " << (config.list_formats ? "yes" : "no") << "\n";
    std::cout << "width              : " << optional_u32_to_string(config.width) << "\n";
    std::cout << "height             : " << optional_u32_to_string(config.height) << "\n";
    std::cout << "format             : " << optional_string_to_string(config.pixel_format) << "\n";
    std::cout << "mmap buffers       : " << optional_u32_to_string(config.mmap_buffers) << "\n";
    std::cout << "capture one        : " << (config.capture_one ? "yes" : "no") << "\n";
    std::cout << "save one           : " << (config.save_one ? "yes" : "no") << "\n";
    std::cout << "capture frames     : " << config.capture_frames << "\n";
    std::cout << "pipeline frames    : " << config.pipeline_frames << "\n";
    std::cout << "ring capacity      : " << config.ring_capacity << "\n";
    std::cout << "tcp queue capacity : " << config.tcp_queue_capacity << "\n";
    std::cout << "consumer delay ms  : " << config.consumer_delay_ms << "\n";
    std::cout << "pipeline save      : " << (config.pipeline_save ? "yes" : "no") << "\n";
    std::cout << "save limit         : " << config.save_limit << "\n";
    std::cout << "output dir         : " << config.output_dir << "\n";
    std::cout << "tcp host           : " << optional_string_to_string(config.tcp_host) << "\n";
    std::cout << "tcp port           : " << config.tcp_port << "\n";
    std::cout << "timeout ms         : " << config.timeout_ms << "\n";
    std::cout << "log level          : " << config.log_level << "\n";
    std::cout << "=========================================" << "\n";
}
