#pragma once

#include "pipeline_stats.hpp"

#include <cstdint>
#include <string>

struct ReceiverStatsSnapshot {
    std::uint64_t received_frames = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t crc_errors = 0;
    std::uint64_t header_errors = 0;
    std::uint64_t rejected_frames = 0;
    std::uint64_t saved_files = 0;
    std::string last_error;
};

std::string json_escape(const std::string& value);
void write_atomic_text_file(const std::string& path, const std::string& text);
void write_pipeline_stats_json(const std::string& path, const PipelineStatsSnapshot& snapshot);
void write_receiver_stats_json(const std::string& path, const ReceiverStatsSnapshot& snapshot);
