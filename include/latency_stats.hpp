#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <vector>

struct LatencySummary {
    std::uint64_t sample_count = 0;
    double min_us = 0.0;
    double mean_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
    double jitter_us = 0.0;
};

class LatencyStats {
public:
    void add_sample_us(double value_us) {
        if (!std::isfinite(value_us) || value_us < 0.0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        samples_us_.push_back(value_us);
    }

    LatencySummary snapshot() const {
        std::vector<double> samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples = samples_us_;
        }

        LatencySummary summary;
        summary.sample_count = static_cast<std::uint64_t>(samples.size());
        if (samples.empty()) {
            return summary;
        }

        std::sort(samples.begin(), samples.end());

        summary.min_us = samples.front();
        summary.max_us = samples.back();
        summary.mean_us = std::accumulate(samples.begin(), samples.end(), 0.0) /
                          static_cast<double>(samples.size());
        summary.p50_us = percentile(samples, 0.50);
        summary.p95_us = percentile(samples, 0.95);
        summary.p99_us = percentile(samples, 0.99);

        double squared_deviation_sum = 0.0;
        for (const double sample : samples) {
            const double delta = sample - summary.mean_us;
            squared_deviation_sum += delta * delta;
        }
        summary.jitter_us = std::sqrt(
            squared_deviation_sum / static_cast<double>(samples.size())
        );

        return summary;
    }

private:
    static double percentile(const std::vector<double>& sorted_samples, double quantile) {
        if (sorted_samples.empty()) {
            return 0.0;
        }
        if (sorted_samples.size() == 1) {
            return sorted_samples.front();
        }

        const double rank = quantile * static_cast<double>(sorted_samples.size() - 1);
        const auto lower = static_cast<std::size_t>(std::floor(rank));
        const auto upper = static_cast<std::size_t>(std::ceil(rank));
        const double fraction = rank - static_cast<double>(lower);

        return sorted_samples[lower] +
               (sorted_samples[upper] - sorted_samples[lower]) * fraction;
    }

    mutable std::mutex mutex_;
    std::vector<double> samples_us_;
};
