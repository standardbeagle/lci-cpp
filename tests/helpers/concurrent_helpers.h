#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace lci {
namespace testing {

// TestResult holds the outcome of a single concurrent operation.
struct TestResult {
    int thread_id = 0;
    int operation_id = 0;
    bool success = false;
    std::string error;
    std::chrono::nanoseconds duration{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
};

// ConcurrentTestResults aggregates results from a concurrent test run.
struct ConcurrentTestResults {
    std::vector<TestResult> results;
    int64_t total_ops = 0;
    int64_t successful_ops = 0;
    int64_t failed_ops = 0;
    std::chrono::nanoseconds total_duration{0};
    std::chrono::nanoseconds avg_op_time{0};
    std::chrono::nanoseconds min_op_time{std::chrono::hours(1)};
    std::chrono::nanoseconds max_op_time{0};
};

// ConcurrentTestScenario defines a concurrent test configuration.
struct ConcurrentTestScenario {
    std::string name;
    int num_threads = 4;
    int ops_per_thread = 10;
    std::function<std::string(int thread_id, int op_id)> operation;
    std::chrono::milliseconds timeout{30000};
};

// RunConcurrentTest executes a concurrent test scenario and collects results.
// The operation function returns an empty string on success or an error message.
inline ConcurrentTestResults RunConcurrentTest(
    const ConcurrentTestScenario& scenario) {

    ConcurrentTestResults results;
    std::mutex mu;

    auto total = scenario.num_threads * scenario.ops_per_thread;
    results.results.reserve(static_cast<size_t>(total));

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(scenario.num_threads));

    for (int tid = 0; tid < scenario.num_threads; ++tid) {
        threads.emplace_back([&, tid] {
            for (int oid = 0; oid < scenario.ops_per_thread; ++oid) {
                TestResult r;
                r.thread_id = tid;
                r.operation_id = oid;
                r.start_time = std::chrono::steady_clock::now();

                r.error = scenario.operation(tid, oid);
                r.success = r.error.empty();

                r.end_time = std::chrono::steady_clock::now();
                r.duration = r.end_time - r.start_time;

                std::lock_guard<std::mutex> lock(mu);
                results.results.push_back(std::move(r));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Compute statistics.
    for (const auto& r : results.results) {
        results.total_duration += r.duration;
        if (r.duration < results.min_op_time) {
            results.min_op_time = r.duration;
        }
        if (r.duration > results.max_op_time) {
            results.max_op_time = r.duration;
        }
        if (r.success) {
            ++results.successful_ops;
        } else {
            ++results.failed_ops;
        }
    }
    results.total_ops = static_cast<int64_t>(results.results.size());
    if (results.total_ops > 0) {
        results.avg_op_time =
            results.total_duration / results.total_ops;
    }

    return results;
}

// AssertConcurrentResults validates that a concurrent test met its criteria.
inline void AssertConcurrentResults(
    const ConcurrentTestResults& results,
    double min_success_rate,
    std::chrono::nanoseconds max_op_time = std::chrono::nanoseconds::zero()) {

    ASSERT_GT(results.total_ops, 0) << "No operations were executed";

    double success_rate =
        static_cast<double>(results.successful_ops) /
        static_cast<double>(results.total_ops);
    EXPECT_GE(success_rate, min_success_rate)
        << "Success rate " << (success_rate * 100.0)
        << "% below minimum " << (min_success_rate * 100.0) << "%";

    if (max_op_time.count() > 0) {
        EXPECT_LE(results.max_op_time, max_op_time)
            << "Max operation time exceeded limit";
    }
}

// RunStressTest runs an operation repeatedly across many threads for a
// specified duration, returning the total number of successful completions.
inline int64_t RunStressTest(
    std::chrono::milliseconds duration,
    int num_threads,
    const std::function<bool()>& operation) {

    std::atomic<bool> stop{false};
    std::atomic<int64_t> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (operation()) {
                    success_count.fetch_add(1,
                                            std::memory_order_relaxed);
                }
            }
        });
    }

    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads) {
        t.join();
    }

    return success_count.load();
}

}  // namespace testing
}  // namespace lci
