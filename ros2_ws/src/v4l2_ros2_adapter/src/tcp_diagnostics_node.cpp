#include "v4l2_ros2_adapter/tcp_diagnostics_node.hpp"

#include "frame_protocol.hpp"
#include "v4l2_ros2_adapter/yuyv_conversion.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace v4l2_ros2_adapter {
namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void add_value(
    diagnostic_msgs::msg::DiagnosticStatus& status,
    const std::string& key,
    const std::string& value
) {
    diagnostic_msgs::msg::KeyValue item;
    item.key = key;
    item.value = value;
    status.values.push_back(std::move(item));
}

std::string to_string_bool(bool value) {
    return value ? "true" : "false";
}

}  // namespace

TcpDiagnosticsNode::TcpDiagnosticsNode(const rclcpp::NodeOptions& options)
    : Node("v4l2_diagnostics_node", options),
      listen_address_(declare_parameter<std::string>("listen_address", "0.0.0.0")),
      listen_port_(declare_parameter<std::int64_t>("listen_port", 9800)),
      max_payload_bytes_(declare_parameter<std::int64_t>(
          "max_payload_bytes",
          static_cast<std::int64_t>(frame_protocol::kDefaultMaxPayloadBytes)
      )),
      publish_period_ms_(declare_parameter<std::int64_t>("publish_period_ms", 1000)),
      socket_poll_timeout_ms_(declare_parameter<std::int64_t>(
          "socket_poll_timeout_ms", 200
      )),
      max_frames_(declare_parameter<std::int64_t>("max_frames", 0)),
      max_sessions_(declare_parameter<std::int64_t>("max_sessions", 0)),
      diagnostic_topic_(declare_parameter<std::string>(
          "diagnostic_topic", "/camera_link/diagnostics"
      )),
      publish_images_(declare_parameter<bool>("publish_images", true)),
      image_topic_(declare_parameter<std::string>("image_topic", "/camera/image_raw")),
      camera_info_topic_(declare_parameter<std::string>(
          "camera_info_topic", "/camera/camera_info"
      )),
      camera_frame_id_(declare_parameter<std::string>(
          "camera_frame_id", "camera_optical_frame"
      )),
      output_encoding_(declare_parameter<std::string>("output_encoding", "rgb8")),
      image_qos_reliability_(declare_parameter<std::string>(
          "image_qos_reliability", "best_effort"
      )),
      image_qos_depth_(declare_parameter<std::int64_t>("image_qos_depth", 5)),
      camera_fx_(declare_parameter<double>("camera_fx", 0.0)),
      camera_fy_(declare_parameter<double>("camera_fy", 0.0)),
      camera_cx_(declare_parameter<double>("camera_cx", 0.0)),
      camera_cy_(declare_parameter<double>("camera_cy", 0.0)),
      camera_k1_(declare_parameter<double>("camera_k1", 0.0)),
      camera_k2_(declare_parameter<double>("camera_k2", 0.0)),
      camera_p1_(declare_parameter<double>("camera_p1", 0.0)),
      camera_p2_(declare_parameter<double>("camera_p2", 0.0)),
      camera_k3_(declare_parameter<double>("camera_k3", 0.0)),
      camera_calibrated_(camera_fx_ > 0.0 && camera_fy_ > 0.0),
      last_rate_time_(std::chrono::steady_clock::now()) {
    validate_parameters();

    diagnostics_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
        diagnostic_topic_,
        rclcpp::QoS(10).reliable()
    );

    if (publish_images_) {
        rclcpp::QoS image_qos(rclcpp::KeepLast(
            static_cast<std::size_t>(image_qos_depth_)
        ));
        image_qos.durability_volatile();
        if (image_qos_reliability_ == "reliable") {
            image_qos.reliable();
        } else {
            image_qos.best_effort();
        }

        image_publisher_ = create_publisher<sensor_msgs::msg::Image>(
            image_topic_,
            image_qos
        );
        camera_info_publisher_ = create_publisher<sensor_msgs::msg::CameraInfo>(
            camera_info_topic_,
            image_qos
        );
    }

    diagnostics_timer_ = create_wall_timer(
        std::chrono::milliseconds(publish_period_ms_),
        std::bind(&TcpDiagnosticsNode::publish_diagnostics, this)
    );

    RCLCPP_INFO(
        get_logger(),
        "starting protocol v%u camera adapter on %s:%d, diagnostics=%s",
        static_cast<unsigned>(frame_protocol::kVersion),
        listen_address_.c_str(),
        static_cast<int>(listen_port_),
        diagnostic_topic_.c_str()
    );
    if (publish_images_) {
        RCLCPP_INFO(
            get_logger(),
            "publishing images on %s (%s), CameraInfo on %s, frame_id=%s, QoS=%s depth=%lld, calibrated=%s",
            image_topic_.c_str(),
            output_encoding_.c_str(),
            camera_info_topic_.c_str(),
            camera_frame_id_.c_str(),
            image_qos_reliability_.c_str(),
            static_cast<long long>(image_qos_depth_),
            camera_calibrated_ ? "true" : "false"
        );
    } else {
        RCLCPP_INFO(get_logger(), "image publication disabled by publish_images=false");
    }

    receiver_thread_ = std::thread(&TcpDiagnosticsNode::receiver_loop, this);
}

TcpDiagnosticsNode::~TcpDiagnosticsNode() {
    stop_requested_.store(true);
    interrupt_sockets();
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
}

void TcpDiagnosticsNode::validate_parameters() const {
    if (listen_address_.empty()) {
        throw std::invalid_argument("listen_address must not be empty");
    }
    if (listen_port_ <= 0 || listen_port_ > 65535) {
        throw std::invalid_argument("listen_port must be between 1 and 65535");
    }
    if (max_payload_bytes_ <= 0 ||
        max_payload_bytes_ > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("max_payload_bytes must fit in a positive uint32 value");
    }
    if (publish_period_ms_ < 50 || publish_period_ms_ > 60000) {
        throw std::invalid_argument("publish_period_ms must be between 50 and 60000");
    }
    if (socket_poll_timeout_ms_ < 10 || socket_poll_timeout_ms_ > 5000) {
        throw std::invalid_argument("socket_poll_timeout_ms must be between 10 and 5000");
    }
    if (max_frames_ < 0) {
        throw std::invalid_argument("max_frames must be non-negative");
    }
    if (max_sessions_ < 0) {
        throw std::invalid_argument("max_sessions must be non-negative");
    }
    if (diagnostic_topic_.empty()) {
        throw std::invalid_argument("diagnostic_topic must not be empty");
    }
    if (publish_images_) {
        if (image_topic_.empty()) {
            throw std::invalid_argument("image_topic must not be empty");
        }
        if (camera_info_topic_.empty()) {
            throw std::invalid_argument("camera_info_topic must not be empty");
        }
        if (camera_frame_id_.empty()) {
            throw std::invalid_argument("camera_frame_id must not be empty");
        }
        if (
            output_encoding_ != "rgb8" &&
            output_encoding_ != "mono8" &&
            output_encoding_ != "yuv422_yuy2"
        ) {
            throw std::invalid_argument(
                "output_encoding must be rgb8, mono8, "
                "or yuv422_yuy2"
            );
        }
        if (image_qos_reliability_ != "best_effort" &&
            image_qos_reliability_ != "reliable") {
            throw std::invalid_argument(
                "image_qos_reliability must be best_effort or reliable"
            );
        }
        if (image_qos_depth_ < 1 || image_qos_depth_ > 1000) {
            throw std::invalid_argument("image_qos_depth must be between 1 and 1000");
        }
    }

    const bool fx_set = camera_fx_ > 0.0;
    const bool fy_set = camera_fy_ > 0.0;
    if (fx_set != fy_set) {
        throw std::invalid_argument(
            "camera_fx and camera_fy must both be positive or both be zero"
        );
    }
    if (camera_cx_ < 0.0 || camera_cy_ < 0.0) {
        throw std::invalid_argument("camera_cx and camera_cy must be non-negative");
    }

    const double calibration_values[] = {
        camera_fx_, camera_fy_, camera_cx_, camera_cy_,
        camera_k1_, camera_k2_, camera_p1_, camera_p2_, camera_k3_
    };
    for (double value : calibration_values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("camera calibration parameters must be finite");
        }
    }
}

void TcpDiagnosticsNode::receiver_loop() {
    int local_listen_fd = -1;

    try {
        local_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (local_listen_fd < 0) {
            throw std::runtime_error(
                std::string("socket failed: ") + std::strerror(errno)
            );
        }
        register_listen_fd(local_listen_fd);

        int reuse = 1;
        if (setsockopt(local_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            throw std::runtime_error(
                std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno)
            );
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(listen_port_));
        if (inet_pton(AF_INET, listen_address_.c_str(), &address.sin_addr) != 1) {
            throw std::invalid_argument("listen_address must be a valid IPv4 address");
        }

        if (bind(
                local_listen_fd,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) < 0) {
            throw std::runtime_error(
                std::string("bind failed: ") + std::strerror(errno)
            );
        }

        if (listen(local_listen_fd, 4) < 0) {
            throw std::runtime_error(
                std::string("listen failed: ") + std::strerror(errno)
            );
        }

        set_connection_state("listening", "waiting for a protocol v3 TCP frame sender");

        while (!stop_requested_.load()) {
            if (max_frames_ > 0 &&
                received_frames_.load() >= static_cast<std::uint64_t>(max_frames_)) {
                set_connection_state("stopped", "configured frame limit reached");
                break;
            }
            if (max_sessions_ > 0 &&
                accepted_sessions_.load() >= static_cast<std::uint64_t>(max_sessions_)) {
                set_connection_state("stopped", "configured session limit reached");
                break;
            }

            pollfd descriptor{};
            descriptor.fd = local_listen_fd;
            descriptor.events = POLLIN;

            const int poll_result = poll(&descriptor, 1, socket_poll_timeout_ms_);
            if (poll_result == 0) {
                continue;
            }
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (stop_requested_.load()) {
                    break;
                }
                throw std::runtime_error(
                    std::string("listen poll failed: ") + std::strerror(errno)
                );
            }
            if ((descriptor.revents & POLLNVAL) != 0) {
                if (stop_requested_.load()) {
                    break;
                }
                throw std::runtime_error("listening socket became invalid");
            }
            if ((descriptor.revents & POLLIN) == 0) {
                continue;
            }

            sockaddr_in peer{};
            socklen_t peer_length = sizeof(peer);
            const int local_client_fd = accept(
                local_listen_fd,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length
            );
            if (local_client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (stop_requested_.load()) {
                    break;
                }
                throw std::runtime_error(
                    std::string("accept failed: ") + std::strerror(errno)
                );
            }

            register_client_fd(local_client_fd);
            const std::uint64_t session_index = accepted_sessions_.fetch_add(1) + 1;

            char peer_text[INET_ADDRSTRLEN]{};
            const char* peer_result = inet_ntop(
                AF_INET,
                &peer.sin_addr,
                peer_text,
                sizeof(peer_text)
            );
            const std::string peer_address = peer_result != nullptr ? peer_text : "unknown";
            set_connection_state(
                "connected",
                "receiving frames from " + peer_address + ":" +
                    std::to_string(ntohs(peer.sin_port))
            );

            RCLCPP_INFO(
                get_logger(),
                "accepted TCP frame session %llu from %s:%u",
                static_cast<unsigned long long>(session_index),
                peer_address.c_str(),
                static_cast<unsigned>(ntohs(peer.sin_port))
            );

            handle_client(local_client_fd, session_index);
            ++completed_sessions_;

            clear_client_fd(local_client_fd);
            close_fd(local_client_fd);

            if (!stop_requested_.load()) {
                set_connection_state("listening", "waiting for the next TCP frame sender");
            }
        }
    } catch (const std::exception& error) {
        if (!stop_requested_.load()) {
            set_last_error(error.what());
            set_connection_state("error", error.what());
            RCLCPP_ERROR(get_logger(), "TCP diagnostics receiver stopped: %s", error.what());
        }
    }

    clear_listen_fd(local_listen_fd);
    close_fd(local_listen_fd);
}

void TcpDiagnosticsNode::handle_client(int client_fd, std::uint64_t session_index) {
    while (!stop_requested_.load()) {
        if (max_frames_ > 0 &&
            received_frames_.load() >= static_cast<std::uint64_t>(max_frames_)) {
            return;
        }

        frame_protocol::FrameHeader header{};
        std::string read_error;
        const ReadResult header_result = read_exact_interruptible(
            client_fd,
            &header,
            sizeof(header),
            read_error
        );

        if (header_result == ReadResult::peer_closed) {
            ++peer_disconnects_;
            RCLCPP_INFO(
                get_logger(),
                "peer closed session %llu",
                static_cast<unsigned long long>(session_index)
            );
            return;
        }
        if (header_result == ReadResult::stopped) {
            return;
        }
        if (header_result == ReadResult::error) {
            ++peer_disconnects_;
            set_last_error(read_error);
            RCLCPP_WARN(
                get_logger(),
                "session %llu header read failed: %s",
                static_cast<unsigned long long>(session_index),
                read_error.c_str()
            );
            return;
        }

        std::string validation_error;
        if (!frame_protocol::validate_header(
                header,
                static_cast<std::uint32_t>(max_payload_bytes_),
                validation_error
            )) {
            ++header_errors_;
            ++rejected_frames_;
            set_last_error(validation_error);
            RCLCPP_WARN(
                get_logger(),
                "rejected frame header in session %llu: %s",
                static_cast<unsigned long long>(session_index),
                validation_error.c_str()
            );
            return;
        }

        std::vector<std::uint8_t> payload(header.payload_size);
        read_error.clear();
        const ReadResult payload_result = read_exact_interruptible(
            client_fd,
            payload.data(),
            payload.size(),
            read_error
        );

        if (payload_result == ReadResult::peer_closed || payload_result == ReadResult::error) {
            ++peer_disconnects_;
            set_last_error(
                read_error.empty() ? "peer closed while receiving frame payload" : read_error
            );
            RCLCPP_WARN(
                get_logger(),
                "session %llu ended while receiving payload for frame %llu",
                static_cast<unsigned long long>(session_index),
                static_cast<unsigned long long>(header.frame_id)
            );
            return;
        }
        if (payload_result == ReadResult::stopped) {
            return;
        }

        const std::uint32_t actual_crc = frame_protocol::compute_crc32(
            payload.data(),
            payload.size()
        );
        if (actual_crc != header.payload_crc32) {
            ++crc_errors_;
            ++rejected_frames_;
            set_last_error(
                "CRC mismatch for frame " + std::to_string(header.frame_id)
            );
            RCLCPP_WARN(
                get_logger(),
                "CRC mismatch for frame %llu: expected=0x%08x actual=0x%08x",
                static_cast<unsigned long long>(header.frame_id),
                header.payload_crc32,
                actual_crc
            );
            continue;
        }

        const std::uint64_t receive_timestamp_ns = system_time_ns();
        if (receive_timestamp_ns >= header.capture_timestamp_ns) {
            const double latency_us = static_cast<double>(
                receive_timestamp_ns - header.capture_timestamp_ns
            ) / 1000.0;
            e2e_latency_.add_sample_us(latency_us);
        } else {
            ++latency_clock_errors_;
            set_last_error(
                "receive clock was earlier than capture timestamp for frame " +
                std::to_string(header.frame_id)
            );
        }

        ++received_frames_;
        received_bytes_.fetch_add(header.payload_size);
        last_frame_id_.store(header.frame_id);
        last_width_.store(header.width);
        last_height_.store(header.height);
        last_pixel_format_.store(header.pixel_format);
        last_capture_timestamp_ns_.store(header.capture_timestamp_ns);
        last_receive_timestamp_ns_.store(receive_timestamp_ns);

        publish_frame(header, payload);
    }
}

void TcpDiagnosticsNode::publish_frame(
    const frame_protocol::FrameHeader& header,
    const std::vector<std::uint8_t>& payload
) {
    if (!publish_images_) {
        return;
    }

    if (header.pixel_format != V4L2_PIX_FMT_YUYV) {
        ++image_publish_errors_;
        set_last_error(
            "ROS image publication supports only YUYV input, got " +
            fourcc_to_string(header.pixel_format)
        );
        return;
    }

    try {
        sensor_msgs::msg::Image image;
        image.header.stamp = to_ros_time(header.capture_timestamp_ns);
        image.header.frame_id = camera_frame_id_;
        image.height = header.height;
        image.width = header.width;
        image.is_bigendian = 0;

        if (output_encoding_ == "rgb8") {
            image.encoding = "rgb8";
            image.step = header.width * 3u;
            image.data = convert_yuyv_to_rgb8(
                payload,
                header.width,
                header.height
            );
        } else if (output_encoding_ == "mono8") {
            image.encoding = "mono8";
            image.step = header.width;
            image.data = convert_yuyv_to_mono8(
                payload,
                header.width,
                header.height
            );
        } else {
            image.encoding = "yuv422_yuy2";
            image.step = header.width * 2u;
            image.data = payload;
        }

        sensor_msgs::msg::CameraInfo camera_info = make_camera_info(
            header.width,
            header.height,
            image.header.stamp
        );

        image_publisher_->publish(image);
        camera_info_publisher_->publish(camera_info);
        ++published_images_;
    } catch (const std::exception& error) {
        ++image_publish_errors_;
        set_last_error(
            "failed to publish ROS image for frame " +
            std::to_string(header.frame_id) + ": " + error.what()
        );
        RCLCPP_WARN(
            get_logger(),
            "failed to publish ROS image for frame %llu: %s",
            static_cast<unsigned long long>(header.frame_id),
            error.what()
        );
    }
}

sensor_msgs::msg::CameraInfo TcpDiagnosticsNode::make_camera_info(
    std::uint32_t width,
    std::uint32_t height,
    const builtin_interfaces::msg::Time& stamp
) const {
    sensor_msgs::msg::CameraInfo info;
    info.header.stamp = stamp;
    info.header.frame_id = camera_frame_id_;
    info.width = width;
    info.height = height;
    info.distortion_model = "plumb_bob";

    info.k.fill(0.0);
    info.r.fill(0.0);
    info.p.fill(0.0);
    info.r[0] = 1.0;
    info.r[4] = 1.0;
    info.r[8] = 1.0;

    if (camera_calibrated_) {
        info.d = {camera_k1_, camera_k2_, camera_p1_, camera_p2_, camera_k3_};

        info.k[0] = camera_fx_;
        info.k[2] = camera_cx_;
        info.k[4] = camera_fy_;
        info.k[5] = camera_cy_;
        info.k[8] = 1.0;

        info.p[0] = camera_fx_;
        info.p[2] = camera_cx_;
        info.p[5] = camera_fy_;
        info.p[6] = camera_cy_;
        info.p[10] = 1.0;
    }

    return info;
}

TcpDiagnosticsNode::ReadResult TcpDiagnosticsNode::read_exact_interruptible(
    int fd,
    void* buffer,
    std::size_t size,
    std::string& error
) const {
    auto* output = static_cast<std::uint8_t*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        if (stop_requested_.load()) {
            return ReadResult::stopped;
        }

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;

        const int poll_result = poll(&descriptor, 1, socket_poll_timeout_ms_);
        if (poll_result == 0) {
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("client poll failed: ") + std::strerror(errno);
            return ReadResult::error;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            error = "client socket became invalid";
            return ReadResult::error;
        }
        if ((descriptor.revents & POLLERR) != 0) {
            error = "client socket reported a poll error";
            return ReadResult::error;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }

        const ssize_t count = recv(fd, output + total, size - total, 0);
        if (count == 0) {
            if (total == 0) {
                return ReadResult::peer_closed;
            }
            error = "peer closed after " + std::to_string(total) +
                    " of " + std::to_string(size) + " requested bytes";
            return ReadResult::error;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("recv failed: ") + std::strerror(errno);
            return ReadResult::error;
        }

        total += static_cast<std::size_t>(count);
    }

    return ReadResult::ok;
}

void TcpDiagnosticsNode::publish_diagnostics() {
    const auto now_steady = std::chrono::steady_clock::now();
    const std::uint64_t frame_count = received_frames_.load();
    const double interval_seconds = std::chrono::duration<double>(
        now_steady - last_rate_time_
    ).count();
    const double receive_fps = interval_seconds > 0.0
        ? static_cast<double>(frame_count - last_rate_frame_count_) / interval_seconds
        : 0.0;
    last_rate_frame_count_ = frame_count;
    last_rate_time_ = now_steady;

    std::string connection_state;
    std::string state_message;
    std::string last_error;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connection_state = connection_state_;
        state_message = state_message_;
        last_error = last_error_;
    }

    const LatencySummary latency = e2e_latency_.snapshot();
    const std::uint64_t crc_errors = crc_errors_.load();
    const std::uint64_t header_errors = header_errors_.load();
    const std::uint64_t clock_errors = latency_clock_errors_.load();
    const std::uint64_t image_errors = image_publish_errors_.load();
    const std::uint64_t total_errors =
        crc_errors + header_errors + clock_errors + image_errors;

    diagnostic_msgs::msg::DiagnosticArray message;
    message.header.stamp = get_clock()->now();

    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "v4l2_capture/camera_link";
    status.hardware_id = "tcp://" + listen_address_ + ":" + std::to_string(listen_port_);

    if (connection_state == "error") {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
    } else if (total_errors > 0 || rejected_frames_.load() > 0) {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
    } else {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    }
    status.message = state_message;

    const char* rmw_environment = std::getenv("RMW_IMPLEMENTATION");
    add_value(
        status,
        "rmw_implementation",
        rmw_environment != nullptr ? rmw_environment : "runtime-default"
    );
    add_value(status, "protocol_version", std::to_string(frame_protocol::kVersion));
    add_value(status, "connection_state", connection_state);
    add_value(status, "connected", to_string_bool(connection_state == "connected"));
    add_value(status, "listen_address", listen_address_);
    add_value(status, "listen_port", std::to_string(listen_port_));
    add_value(status, "publish_images", to_string_bool(publish_images_));
    add_value(status, "image_topic", image_topic_);
    add_value(status, "camera_info_topic", camera_info_topic_);
    add_value(status, "camera_frame_id", camera_frame_id_);
    add_value(status, "output_encoding", output_encoding_);
    add_value(status, "image_qos_reliability", image_qos_reliability_);
    add_value(status, "image_qos_depth", std::to_string(image_qos_depth_));
    add_value(status, "camera_calibrated", to_string_bool(camera_calibrated_));
    add_value(status, "receive_fps", format_double(receive_fps));
    add_value(status, "received_frames", std::to_string(frame_count));
    add_value(status, "received_bytes", std::to_string(received_bytes_.load()));
    add_value(status, "accepted_sessions", std::to_string(accepted_sessions_.load()));
    add_value(status, "completed_sessions", std::to_string(completed_sessions_.load()));
    add_value(status, "peer_disconnects", std::to_string(peer_disconnects_.load()));
    add_value(status, "header_errors", std::to_string(header_errors));
    add_value(status, "crc_errors", std::to_string(crc_errors));
    add_value(status, "rejected_frames", std::to_string(rejected_frames_.load()));
    add_value(status, "published_images", std::to_string(published_images_.load()));
    add_value(status, "image_publish_errors", std::to_string(image_errors));
    add_value(status, "latency_clock_errors", std::to_string(clock_errors));
    add_value(status, "e2e_latency_samples", std::to_string(latency.sample_count));
    add_value(status, "e2e_latency_mean_us", format_double(latency.mean_us));
    add_value(status, "e2e_latency_p50_us", format_double(latency.p50_us));
    add_value(status, "e2e_latency_p95_us", format_double(latency.p95_us));
    add_value(status, "e2e_latency_p99_us", format_double(latency.p99_us));
    add_value(status, "e2e_latency_max_us", format_double(latency.max_us));
    add_value(status, "e2e_latency_jitter_us", format_double(latency.jitter_us));
    add_value(status, "last_frame_id", std::to_string(last_frame_id_.load()));
    add_value(status, "last_width", std::to_string(last_width_.load()));
    add_value(status, "last_height", std::to_string(last_height_.load()));
    add_value(status, "last_pixel_format", fourcc_to_string(last_pixel_format_.load()));
    add_value(
        status,
        "last_capture_timestamp_ns",
        std::to_string(last_capture_timestamp_ns_.load())
    );
    add_value(
        status,
        "last_receive_timestamp_ns",
        std::to_string(last_receive_timestamp_ns_.load())
    );
    add_value(status, "last_error", last_error);

    message.status.push_back(std::move(status));
    diagnostics_publisher_->publish(message);
}

void TcpDiagnosticsNode::set_connection_state(
    const std::string& state,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    connection_state_ = state;
    state_message_ = message;
}

void TcpDiagnosticsNode::set_last_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = message;
}

void TcpDiagnosticsNode::interrupt_sockets() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (client_fd_ >= 0) {
        shutdown(client_fd_, SHUT_RDWR);
    }
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
    }
}

void TcpDiagnosticsNode::register_listen_fd(int fd) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    listen_fd_ = fd;
}

void TcpDiagnosticsNode::register_client_fd(int fd) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    client_fd_ = fd;
}

void TcpDiagnosticsNode::clear_listen_fd(int fd) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (listen_fd_ == fd) {
        listen_fd_ = -1;
    }
}

void TcpDiagnosticsNode::clear_client_fd(int fd) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (client_fd_ == fd) {
        client_fd_ = -1;
    }
}

std::uint64_t TcpDiagnosticsNode::system_time_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

builtin_interfaces::msg::Time TcpDiagnosticsNode::to_ros_time(
    std::uint64_t timestamp_ns
) {
    builtin_interfaces::msg::Time stamp;
    stamp.sec = static_cast<std::int32_t>(timestamp_ns / 1000000000ull);
    stamp.nanosec = static_cast<std::uint32_t>(timestamp_ns % 1000000000ull);
    return stamp;
}

std::string TcpDiagnosticsNode::fourcc_to_string(std::uint32_t fourcc) {
    if (fourcc == 0) {
        return "unset";
    }

    std::string text;
    text.reserve(4);
    text.push_back(static_cast<char>(fourcc & 0xffu));
    text.push_back(static_cast<char>((fourcc >> 8u) & 0xffu));
    text.push_back(static_cast<char>((fourcc >> 16u) & 0xffu));
    text.push_back(static_cast<char>((fourcc >> 24u) & 0xffu));
    return text;
}

std::string TcpDiagnosticsNode::format_double(double value) {
    if (!std::isfinite(value)) {
        return "0.000";
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << value;
    return output.str();
}

}  // namespace v4l2_ros2_adapter
