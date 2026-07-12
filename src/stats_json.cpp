#include "stats_json.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

void write_latency_fields(
    std::ostringstream& out,
    const std::string& prefix,
    const LatencySummary& summary,
    bool trailing_comma
) {
    out << "  \"" << prefix << "_samples\": " << summary.sample_count << ",\n";
    out << std::fixed << std::setprecision(3);
    out << "  \"" << prefix << "_min_us\": " << summary.min_us << ",\n";
    out << "  \"" << prefix << "_mean_us\": " << summary.mean_us << ",\n";
    out << "  \"" << prefix << "_p50_us\": " << summary.p50_us << ",\n";
    out << "  \"" << prefix << "_p95_us\": " << summary.p95_us << ",\n";
    out << "  \"" << prefix << "_p99_us\": " << summary.p99_us << ",\n";
    out << "  \"" << prefix << "_max_us\": " << summary.max_us << ",\n";
    out << "  \"" << prefix << "_jitter_us\": " << summary.jitter_us;
    out << (trailing_comma ? ",\n" : "\n");
}

}  // namespace

std::string json_escape(const std::string& value) {
    std::ostringstream out;

    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }

    return out.str();
}

void write_atomic_text_file(const std::string& path, const std::string& text) {
    const std::filesystem::path target(path);
    if (target.empty()) {
        throw std::runtime_error("stats output path must not be empty");
    }

    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const std::filesystem::path temp = target.string() + ".tmp";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Failed to open temporary stats file: " + temp.string());
        }

        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            throw std::runtime_error("Failed to write temporary stats file: " + temp.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
    }

    if (ec) {
        throw std::runtime_error(
            "Failed to atomically replace stats file " + target.string() + ": " + ec.message()
        );
    }
}

void write_pipeline_stats_json(const std::string& path, const PipelineStatsSnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"produced_frames\": " << snapshot.captured << ",\n";
    out << "  \"consumed_frames\": " << snapshot.consumed << ",\n";
    out << "  \"invalid_frames\": " << snapshot.invalid << ",\n";
    out << "  \"saved_frames\": " << snapshot.saved << ",\n";
    out << "  \"consumed_bytes\": " << snapshot.consumed_bytes << ",\n";
    out << "  \"ring_dropped_frames\": " << snapshot.capture_dropped << ",\n";
    out << "  \"remaining_ring_size\": " << snapshot.capture_queue_remaining << ",\n";
    out << "  \"tcp_queued_frames\": " << snapshot.tcp_queued << ",\n";
    out << "  \"tcp_queue_dropped\": " << snapshot.tcp_dropped << ",\n";
    out << "  \"tcp_queue_remaining\": " << snapshot.tcp_queue_remaining << ",\n";
    out << "  \"tcp_sent_frames\": " << snapshot.tcp_sent << ",\n";
    out << "  \"tcp_sent_bytes\": " << snapshot.tcp_sent_bytes << ",\n";
    out << "  \"tcp_send_errors\": " << snapshot.tcp_send_errors << ",\n";
    out << "  \"tcp_connect_attempts\": " << snapshot.tcp_connect_attempts << ",\n";
    out << "  \"tcp_connect_retries\": " << snapshot.tcp_connect_retries << ",\n";
    out << "  \"received_frames\": " << snapshot.received << ",\n";
    out << "  \"reconnect_count\": " << snapshot.reconnect_count << ",\n";
    out << std::fixed << std::setprecision(3);
    out << "  \"elapsed_seconds\": " << snapshot.elapsed_seconds << ",\n";
    out << "  \"producer_fps\": " << snapshot.producer_fps << ",\n";
    out << "  \"consumer_fps\": " << snapshot.consumer_fps << ",\n";
    write_latency_fields(out, "capture_to_consumer", snapshot.capture_to_consumer_latency, true);
    write_latency_fields(out, "capture_to_send", snapshot.capture_to_send_latency, true);
    out << "  \"last_error\": \"" << json_escape(snapshot.last_error) << "\"\n";
    out << "}\n";

    write_atomic_text_file(path, out.str());
}

void write_receiver_stats_json(const std::string& path, const ReceiverStatsSnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"received_frames\": " << snapshot.received_frames << ",\n";
    out << "  \"received_bytes\": " << snapshot.received_bytes << ",\n";
    out << "  \"crc_errors\": " << snapshot.crc_errors << ",\n";
    out << "  \"header_errors\": " << snapshot.header_errors << ",\n";
    out << "  \"rejected_frames\": " << snapshot.rejected_frames << ",\n";
    out << "  \"saved_files\": " << snapshot.saved_files << ",\n";
    out << "  \"accepted_sessions\": " << snapshot.accepted_sessions << ",\n";
    out << "  \"completed_sessions\": " << snapshot.completed_sessions << ",\n";
    out << "  \"peer_disconnects\": " << snapshot.peer_disconnects << ",\n";
    out << "  \"latency_clock_errors\": " << snapshot.latency_clock_errors << ",\n";
    write_latency_fields(out, "e2e_latency", snapshot.e2e_latency, true);
    out << "  \"last_error\": \"" << json_escape(snapshot.last_error) << "\"\n";
    out << "}\n";

    write_atomic_text_file(path, out.str());
}
