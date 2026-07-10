#pragma once

#include <sstream>
#include <string>
#include <utility>

// Lightweight thread-safe logger for this project.
// Keep it intentionally small: no external dependency, no async queue, no log rotation.

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

LogLevel parse_log_level(const std::string& level_text);
std::string log_level_to_string(LogLevel level);

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const;

    void log(LogLevel level, const std::string& module, const std::string& message);
    void debug(const std::string& module, const std::string& message);
    void info(const std::string& module, const std::string& message);
    void warn(const std::string& module, const std::string& message);
    void error(const std::string& module, const std::string& message);

private:
    Logger() = default;
};

template <typename... Args>
std::string make_log_message(Args&&... args) {
    std::ostringstream oss;
    (oss << ... << args);
    return oss.str();
}

template <typename... Args>
void log_debug(const std::string& module, Args&&... args) {
    Logger::instance().debug(module, make_log_message(std::forward<Args>(args)...));
}

template <typename... Args>
void log_info(const std::string& module, Args&&... args) {
    Logger::instance().info(module, make_log_message(std::forward<Args>(args)...));
}

template <typename... Args>
void log_warn(const std::string& module, Args&&... args) {
    Logger::instance().warn(module, make_log_message(std::forward<Args>(args)...));
}

template <typename... Args>
void log_error(const std::string& module, Args&&... args) {
    Logger::instance().error(module, make_log_message(std::forward<Args>(args)...));
}
