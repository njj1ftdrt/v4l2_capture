#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/qos.hpp>
#include <sensor_msgs/msg/image.hpp>

class ImageThroughputCheck : public rclcpp::Node
{
public:
    using Clock = std::chrono::steady_clock;

    ImageThroughputCheck()
        : Node("image_throughput_check")
    {
        topic_ = declare_parameter<std::string>(
            "topic", "/camera/image_raw");

        warmup_seconds_ = declare_parameter<double>(
            "warmup_seconds", 2.0);

        measurement_seconds_ = declare_parameter<double>(
            "measurement_seconds", 15.0);

        expected_width_ = declare_parameter<int64_t>(
            "expected_width", 640);

        expected_height_ = declare_parameter<int64_t>(
            "expected_height", 360);

        expected_encoding_ = declare_parameter<std::string>(
            "expected_encoding", "rgb8");

        output_path_ = declare_parameter<std::string>(
            "output_path", "");

        rclcpp::SensorDataQoS qos;
        qos.keep_last(1);
        qos.best_effort();
        qos.durability_volatile();

        subscription_ =
            create_subscription<sensor_msgs::msg::Image>(
                topic_,
                qos,
                std::bind(
                    &ImageThroughputCheck::on_image,
                    this,
                    std::placeholders::_1));

        start_time_ = Clock::now();

        warmup_end_time_ =
            start_time_ +
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(
                    warmup_seconds_));

        measurement_end_time_ =
            warmup_end_time_ +
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(
                    measurement_seconds_));

        timer_ = create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(
                &ImageThroughputCheck::check_deadline,
                this));

        RCLCPP_INFO(
            get_logger(),
            "topic=%s warmup=%.3fs measurement=%.3fs "
            "QoS=BEST_EFFORT/KEEP_LAST(1)",
            topic_.c_str(),
            warmup_seconds_,
            measurement_seconds_);
    }

    int exit_code() const
    {
        return exit_code_;
    }

private:
    void on_image(
        const sensor_msgs::msg::Image::SharedPtr message)
    {
        const auto now = Clock::now();

        if (now < warmup_end_time_) {
            ++warmup_images_;
            return;
        }

        if (now >= measurement_end_time_ || finished_) {
            return;
        }

        if (measured_images_ == 0) {
            first_receive_time_ = now;
        }

        last_receive_time_ = now;
        ++measured_images_;
        total_bytes_ += message->data.size();

        bool valid = true;

        if (message->width !=
            static_cast<uint32_t>(expected_width_)) {
            valid = false;
        }

        if (message->height !=
            static_cast<uint32_t>(expected_height_)) {
            valid = false;
        }

        if (message->encoding != expected_encoding_) {
            valid = false;
        }

        const std::size_t expected_payload =
            static_cast<std::size_t>(message->step) *
            static_cast<std::size_t>(message->height);

        if (message->data.size() != expected_payload) {
            valid = false;
        }

        if (valid) {
            ++valid_images_;
        } else {
            ++invalid_images_;
        }
    }

    void check_deadline()
    {
        if (!finished_ &&
            Clock::now() >= measurement_end_time_) {
            finish();
        }
    }

    static std::string json_escape(
        const std::string &input)
    {
        std::string output;

        for (const char character : input) {
            switch (character) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output += character;
                break;
            }
        }

        return output;
    }

    void write_json(
        double receive_rate_hz,
        double window_rate_hz,
        double callback_span_seconds,
        bool passed)
    {
        if (output_path_.empty()) {
            return;
        }

        const std::filesystem::path output(output_path_);

        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(
                output.parent_path());
        }

        std::filesystem::path temporary = output;
        temporary += ".tmp";

        std::ofstream stream(temporary);

        if (!stream) {
            RCLCPP_ERROR(
                get_logger(),
                "failed to open output path: %s",
                temporary.string().c_str());
            return;
        }

        stream << std::fixed << std::setprecision(6);
        stream << "{\n";
        stream << "  \"topic\": \""
               << json_escape(topic_) << "\",\n";
        stream << "  \"qos_reliability\": "
               << "\"BEST_EFFORT\",\n";
        stream << "  \"qos_depth\": 1,\n";
        stream << "  \"warmup_seconds\": "
               << warmup_seconds_ << ",\n";
        stream << "  \"measurement_seconds\": "
               << measurement_seconds_ << ",\n";
        stream << "  \"warmup_images\": "
               << warmup_images_ << ",\n";
        stream << "  \"measured_images\": "
               << measured_images_ << ",\n";
        stream << "  \"valid_images\": "
               << valid_images_ << ",\n";
        stream << "  \"invalid_images\": "
               << invalid_images_ << ",\n";
        stream << "  \"total_bytes\": "
               << total_bytes_ << ",\n";
        stream << "  \"callback_span_seconds\": "
               << callback_span_seconds << ",\n";
        stream << "  \"receive_rate_hz\": "
               << receive_rate_hz << ",\n";
        stream << "  \"window_rate_hz\": "
               << window_rate_hz << ",\n";
        stream << "  \"expected_width\": "
               << expected_width_ << ",\n";
        stream << "  \"expected_height\": "
               << expected_height_ << ",\n";
        stream << "  \"expected_encoding\": \""
               << json_escape(expected_encoding_) << "\",\n";
        stream << "  \"passed\": "
               << (passed ? "true" : "false") << "\n";
        stream << "}\n";
        stream.close();

        std::error_code error;
        std::filesystem::remove(output, error);
        error.clear();

        std::filesystem::rename(
            temporary,
            output,
            error);

        if (error) {
            RCLCPP_ERROR(
                get_logger(),
                "failed to publish JSON output: %s",
                error.message().c_str());
        }
    }

    void finish()
    {
        finished_ = true;

        double callback_span_seconds = 0.0;
        double receive_rate_hz = 0.0;

        if (measured_images_ >= 2) {
            callback_span_seconds =
                std::chrono::duration<double>(
                    last_receive_time_ -
                    first_receive_time_)
                    .count();

            if (callback_span_seconds > 0.0) {
                receive_rate_hz =
                    static_cast<double>(
                        measured_images_ - 1) /
                    callback_span_seconds;
            }
        }

        const double window_rate_hz =
            measurement_seconds_ > 0.0
                ? static_cast<double>(
                      measured_images_) /
                      measurement_seconds_
                : 0.0;

        const bool passed =
            measured_images_ >= 2 &&
            valid_images_ == measured_images_ &&
            invalid_images_ == 0;

        exit_code_ = passed ? 0 : 1;

        std::cout << std::fixed
                  << std::setprecision(3)
                  << "\n========== C++ Image Throughput ==========\n"
                  << "topic                : "
                  << topic_ << '\n'
                  << "warmup images        : "
                  << warmup_images_ << '\n'
                  << "measured images      : "
                  << measured_images_ << '\n'
                  << "valid images         : "
                  << valid_images_ << '\n'
                  << "invalid images       : "
                  << invalid_images_ << '\n'
                  << "callback span        : "
                  << callback_span_seconds
                  << " s\n"
                  << "receive rate         : "
                  << receive_rate_hz
                  << " Hz\n"
                  << "fixed-window rate    : "
                  << window_rate_hz
                  << " Hz\n"
                  << "QoS                   : "
                  << "BEST_EFFORT / KEEP_LAST(1)\n"
                  << "result                : "
                  << (passed ? "PASS" : "FAIL")
                  << '\n'
                  << "==========================================\n"
                  << std::flush;

        write_json(
            receive_rate_hz,
            window_rate_hz,
            callback_span_seconds,
            passed);

        rclcpp::shutdown();
    }

    std::string topic_;
    std::string expected_encoding_;
    std::string output_path_;

    double warmup_seconds_{2.0};
    double measurement_seconds_{15.0};

    int64_t expected_width_{640};
    int64_t expected_height_{360};

    uint64_t warmup_images_{0};
    uint64_t measured_images_{0};
    uint64_t valid_images_{0};
    uint64_t invalid_images_{0};
    uint64_t total_bytes_{0};

    bool finished_{false};
    int exit_code_{1};

    Clock::time_point start_time_;
    Clock::time_point warmup_end_time_;
    Clock::time_point measurement_end_time_;
    Clock::time_point first_receive_time_;
    Clock::time_point last_receive_time_;

    rclcpp::Subscription<
        sensor_msgs::msg::Image>::SharedPtr
        subscription_;

    rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);

    auto node =
        std::make_shared<ImageThroughputCheck>();

    rclcpp::spin(node);

    const int result = node->exit_code();

    if (rclcpp::ok()) {
        rclcpp::shutdown();
    }

    return result;
}
