#include "latency_stats.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expect_near(double actual, double expected, double tolerance, const char* name) {
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << "[FAIL] " << name
                  << ": actual=" << actual
                  << " expected=" << expected
                  << " tolerance=" << tolerance << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    LatencyStats stats;
    stats.add_sample_us(10.0);
    stats.add_sample_us(20.0);
    stats.add_sample_us(30.0);
    stats.add_sample_us(40.0);
    stats.add_sample_us(50.0);
    stats.add_sample_us(-1.0);

    const LatencySummary summary = stats.snapshot();

    if (summary.sample_count != 5) {
        std::cerr << "[FAIL] sample_count: " << summary.sample_count << "\n";
        return 1;
    }

    expect_near(summary.min_us, 10.0, 1e-9, "min");
    expect_near(summary.mean_us, 30.0, 1e-9, "mean");
    expect_near(summary.p50_us, 30.0, 1e-9, "p50");
    expect_near(summary.p95_us, 48.0, 1e-9, "p95");
    expect_near(summary.p99_us, 49.6, 1e-9, "p99");
    expect_near(summary.max_us, 50.0, 1e-9, "max");
    expect_near(summary.jitter_us, std::sqrt(200.0), 1e-9, "jitter");

    std::cout << "Latency statistics tests passed.\n";
    return 0;
}
