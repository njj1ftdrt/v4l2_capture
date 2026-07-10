#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {

std::mutex& logger_mutex() {
    static std::mutex m;
    return m;
}

LogLevel& current_level() {
    static LogLevel level = LogLevel::INFO;
    return level;
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;

    std::tm tm_now{};
#if defined(_WIN32)
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

std::ostream& stream_for_level(LogLevel level) {
    if (level == LogLevel::ERROR || level == LogLevel::WARN) {
        return std::cerr;
    }
    return std::cout;
}

}  // namespace

LogLevel parse_log_level(const std::string& level_text) {
    const std::string level = to_upper(level_text);
    if (level == "DEBUG") {
        return LogLevel::DEBUG;
    }
    if (level == "INFO") {
        return LogLevel::INFO;
    }
    if (level == "WARN" || level == "WARNING") {
        return LogLevel::WARN;
    }
    if (level == "ERROR") {
        return LogLevel::ERROR;
    }

    throw std::runtime_error("Unknown log level: " + level_text);
}

std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    current_level() = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(logger_mutex());
    return current_level();
}

void Logger::log(LogLevel level, const std::string& module, const std::string& message) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    if (static_cast<int>(level) < static_cast<int>(current_level())) {
        return;
    }

    auto& out = stream_for_level(level);
    out << '[' << current_timestamp() << ']'
        << '[' << log_level_to_string(level) << ']'
        << '[' << module << "] "
        << message << '\n';
}

void Logger::debug(const std::string& module, const std::string& message) {
    log(LogLevel::DEBUG, module, message);
}

void Logger::info(const std::string& module, const std::string& message) {
    log(LogLevel::INFO, module, message);
}

void Logger::warn(const std::string& module, const std::string& message) {
    log(LogLevel::WARN, module, message);
}

void Logger::error(const std::string& module, const std::string& message) {
    log(LogLevel::ERROR, module, message);
}
