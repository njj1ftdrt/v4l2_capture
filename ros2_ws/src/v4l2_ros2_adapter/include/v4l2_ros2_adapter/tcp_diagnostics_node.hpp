#pragma once

#include "frame_protocol.hpp"
#include "latency_stats.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace v4l2_ros2_adapter {

class TcpDiagnosticsNode final : public rclcpp::Node {
public:
    explicit TcpDiagnosticsNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
    ~TcpDiagnosticsNode() override;

private:
    enum class ReadResult {
        ok,
        peer_closed,
        stopped,
        error,
    };

    void validate_parameters() const;
    void receiver_loop();
    void handle_client(int client_fd, std::uint64_t session_index);
    ReadResult read_exact_interruptible(
        int fd,
        void* buffer,
        std::size_t size,
        std::string& error
    ) const;

    void publish_frame(
        const frame_protocol::FrameHeader& header,
        const std::vector<std::uint8_t>& payload
    );
    sensor_msgs::msg::CameraInfo make_camera_info(
        std::uint32_t width,
        std::uint32_t height,
        const builtin_interfaces::msg::Time& stamp
    ) const;

    void publish_diagnostics();
    void set_connection_state(const std::string& state, const std::string& message);
    void set_last_error(const std::string& message);
    void interrupt_sockets();
    void register_listen_fd(int fd);
    void register_client_fd(int fd);
    void clear_listen_fd(int fd);
    void clear_client_fd(int fd);

    static std::uint64_t system_time_ns();
    static builtin_interfaces::msg::Time to_ros_time(std::uint64_t timestamp_ns);
    static std::string fourcc_to_string(std::uint32_t fourcc);
    static std::string format_double(double value);

    const std::string listen_address_;
    const std::int64_t listen_port_;
    const std::int64_t max_payload_bytes_;
    const std::int64_t publish_period_ms_;
    const std::int64_t socket_poll_timeout_ms_;
    const std::int64_t max_frames_;
    const std::int64_t max_sessions_;
    const std::string diagnostic_topic_;

    const bool publish_images_;
    const std::string image_topic_;
    const std::string camera_info_topic_;
    const std::string camera_frame_id_;
    const std::string output_encoding_;
    const std::string image_qos_reliability_;
    const std::int64_t image_qos_depth_;

    const double camera_fx_;
    const double camera_fy_;
    const double camera_cx_;
    const double camera_cy_;
    const double camera_k1_;
    const double camera_k2_;
    const double camera_p1_;
    const double camera_p2_;
    const double camera_k3_;
    const bool camera_calibrated_;

    rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_publisher_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr image_publisher_;
    rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr camera_info_publisher_;
    rclcpp::TimerBase::SharedPtr diagnostics_timer_;
    std::thread receiver_thread_;
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex socket_mutex_;
    int listen_fd_{-1};
    int client_fd_{-1};

    mutable std::mutex state_mutex_;
    std::string connection_state_{"starting"};
    std::string state_message_{"initializing TCP camera adapter"};
    std::string last_error_;

    std::atomic<std::uint64_t> received_frames_{0};
    std::atomic<std::uint64_t> received_bytes_{0};
    std::atomic<std::uint64_t> crc_errors_{0};
    std::atomic<std::uint64_t> header_errors_{0};
    std::atomic<std::uint64_t> rejected_frames_{0};
    std::atomic<std::uint64_t> accepted_sessions_{0};
    std::atomic<std::uint64_t> completed_sessions_{0};
    std::atomic<std::uint64_t> peer_disconnects_{0};
    std::atomic<std::uint64_t> latency_clock_errors_{0};
    std::atomic<std::uint64_t> published_images_{0};
    std::atomic<std::uint64_t> image_publish_errors_{0};
    std::atomic<std::uint64_t> last_frame_id_{0};
    std::atomic<std::uint32_t> last_width_{0};
    std::atomic<std::uint32_t> last_height_{0};
    std::atomic<std::uint32_t> last_pixel_format_{0};
    std::atomic<std::uint64_t> last_capture_timestamp_ns_{0};
    std::atomic<std::uint64_t> last_receive_timestamp_ns_{0};

    LatencyStats e2e_latency_;

    std::uint64_t last_rate_frame_count_{0};
    std::chrono::steady_clock::time_point last_rate_time_;
};

}  // namespace v4l2_ros2_adapter
