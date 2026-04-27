#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace lci {
namespace testing {

// PerformanceThreshold defines acceptable timing bounds for an operation.
struct PerformanceThreshold {
    std::chrono::nanoseconds max_duration{0};
    std::chrono::nanoseconds p50_threshold{0};
    std::chrono::nanoseconds p95_threshold{0};
    std::chrono::nanoseconds p99_threshold{0};
};

// PerformanceResult holds the outcome of a performance check.
struct PerformanceResult {
    std::string name;
    bool passed = true;
    std::chrono::nanoseconds avg_duration{0};
    std::chrono::nanoseconds p50{0};
    std::chrono::nanoseconds p95{0};
    std::chrono::nanoseconds p99{0};
    std::vector<std::string> violations;
};

// PerformanceScaler adjusts thresholds for constrained environments (CI, etc).
struct PerformanceScaler {
    int cpu_count = 0;
    bool is_ci = false;

    PerformanceScaler() : cpu_count(static_cast<int>(
                              std::thread::hardware_concurrency())),
                          is_ci(detect_ci()) {}

    [[nodiscard]] double scale_duration(double base) const {
        double scaled = base;
        if (is_ci) {
            scaled *= 1.5;
        }
        if (cpu_count <= 1) {
            scaled *= 1.5;
        }
        return scaled;
    }

    [[nodiscard]] int scale_iterations(int base) const {
        if (is_ci) {
            return base / 2;
        }
        return base;
    }

  private:
    static bool detect_ci() {
        for (const char* var : {"CI", "CONTINUOUS_INTEGRATION",
                                "GITHUB_ACTIONS", "GITLAB_CI",
                                "JENKINS"}) {
            if (std::getenv(var) != nullptr) {
                return true;
            }
        }
        return false;
    }
};

// PerformanceGuard monitors and enforces performance constraints.
class PerformanceGuard {
  public:
    PerformanceGuard() = default;

    void add_threshold(const std::string& name,
                       PerformanceThreshold threshold) {
        thresholds_[name] = threshold;
    }

    void add_search_threshold() {
        PerformanceScaler scaler;
        using ms = std::chrono::milliseconds;
        auto scale = [&](ms d) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double, std::milli>(
                    static_cast<double>(d.count()) *
                    scaler.scale_duration(1.0)));
        };
        add_threshold("search", {
            .max_duration = scale(ms(5)),
            .p50_threshold = scale(ms(2)),
            .p95_threshold = scale(ms(5)),
            .p99_threshold = scale(ms(10)),
        });
    }

    // Measure runs fn once and records its duration under name.
    std::chrono::nanoseconds measure(const std::string& name,
                                     const std::function<void()>& fn) {
        auto start = std::chrono::steady_clock::now();
        fn();
        auto dur = std::chrono::steady_clock::now() - start;

        std::lock_guard<std::mutex> lock(mu_);
        measurements_[name].push_back(dur);
        return dur;
    }

    // MeasureN runs fn n times with a small warm-up and records durations.
    void measure_n(const std::string& name, int n,
                   const std::function<void()>& fn) {
        int warmup = std::min(3, n / 10 + 1);
        for (int i = 0; i < warmup; ++i) {
            fn();
        }
        for (int i = 0; i < n; ++i) {
            measure(name, fn);
        }
    }

    // Check evaluates measurements against the registered threshold.
    [[nodiscard]] PerformanceResult check(const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);

        PerformanceResult result;
        result.name = name;

        auto thr_it = thresholds_.find(name);
        if (thr_it == thresholds_.end()) {
            return result;
        }
        const auto& thr = thr_it->second;

        auto m_it = measurements_.find(name);
        if (m_it == measurements_.end() || m_it->second.empty()) {
            result.passed = false;
            result.violations.emplace_back("No measurements recorded");
            return result;
        }

        auto durations = m_it->second;
        std::sort(durations.begin(), durations.end());

        result.avg_duration = average(durations);
        result.p50 = percentile(durations, 50);
        result.p95 = percentile(durations, 95);
        result.p99 = percentile(durations, 99);

        if (thr.max_duration.count() > 0 &&
            result.avg_duration > thr.max_duration) {
            result.violations.push_back(
                "Average duration exceeded max threshold");
        }
        if (thr.p50_threshold.count() > 0 &&
            result.p50 > thr.p50_threshold) {
            result.violations.push_back("P50 exceeded threshold");
        }
        if (thr.p95_threshold.count() > 0 &&
            result.p95 > thr.p95_threshold) {
            result.violations.push_back("P95 exceeded threshold");
        }
        if (thr.p99_threshold.count() > 0 &&
            result.p99 > thr.p99_threshold) {
            result.violations.push_back("P99 exceeded threshold");
        }

        result.passed = result.violations.empty();
        return result;
    }

    void assert_passed(const std::string& name) {
        auto result = check(name);
        if (!result.passed) {
            std::string msg = "Performance check failed for " + name + ":";
            for (const auto& v : result.violations) {
                msg += "\n  - " + v;
            }
            FAIL() << msg;
        }
    }

    void assert_all_passed() {
        for (const auto& [name, _] : thresholds_) {
            assert_passed(name);
        }
    }

  private:
    std::mutex mu_;
    std::unordered_map<std::string, PerformanceThreshold> thresholds_;
    std::unordered_map<std::string, std::vector<std::chrono::nanoseconds>>
        measurements_;

    static std::chrono::nanoseconds average(
        const std::vector<std::chrono::nanoseconds>& durations) {
        if (durations.empty()) return std::chrono::nanoseconds{0};
        std::chrono::nanoseconds sum{0};
        for (auto d : durations) sum += d;
        return sum / static_cast<int64_t>(durations.size());
    }

    static std::chrono::nanoseconds percentile(
        const std::vector<std::chrono::nanoseconds>& sorted_durations,
        int p) {
        if (sorted_durations.empty()) return std::chrono::nanoseconds{0};
        auto idx = static_cast<size_t>(
            (p * static_cast<int>(sorted_durations.size())) / 100);
        if (idx >= sorted_durations.size()) {
            idx = sorted_durations.size() - 1;
        }
        return sorted_durations[idx];
    }
};

// ThroughputGuard monitors operation throughput (ops/sec).
class ThroughputGuard {
  public:
    explicit ThroughputGuard(double min_ops_per_sec)
        : min_ops_per_sec_(min_ops_per_sec) {}

    double measure_ops(int ops, const std::function<void()>& fn) {
        auto start = std::chrono::steady_clock::now();
        fn();
        auto dur = std::chrono::steady_clock::now() - start;

        double secs =
            std::chrono::duration<double>(dur).count();
        double throughput = static_cast<double>(ops) / secs;

        std::lock_guard<std::mutex> lock(mu_);
        measurements_.push_back(throughput);
        return throughput;
    }

    [[nodiscard]] double average_throughput() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (measurements_.empty()) return 0.0;
        double sum = 0.0;
        for (double m : measurements_) sum += m;
        return sum / static_cast<double>(measurements_.size());
    }

    void assert_min_throughput() {
        double avg = average_throughput();
        EXPECT_GE(avg, min_ops_per_sec_)
            << "Throughput " << avg
            << " ops/sec below minimum " << min_ops_per_sec_;
    }

  private:
    double min_ops_per_sec_;
    mutable std::mutex mu_;
    std::vector<double> measurements_;
};

}  // namespace testing
}  // namespace lci
