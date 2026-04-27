#include <gtest/gtest.h>

#include "concurrent_helpers.h"
#include "isolated_test_env.h"
#include "performance_guards.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace lci {
namespace testing {
namespace {

// ---------------------------------------------------------------------------
// IsolatedTestEnv
// ---------------------------------------------------------------------------
TEST(IsolatedTestEnvTest, CreatesAndCleansTemp) {
    std::filesystem::path dir;
    {
        IsolatedTestEnv env;
        dir = env.temp_dir();
        ASSERT_TRUE(std::filesystem::exists(dir));
    }
    EXPECT_FALSE(std::filesystem::exists(dir));
}

TEST(IsolatedTestEnvTest, WriteAndReadFile) {
    IsolatedTestEnv env;
    env.write_file("sub/file.txt", "hello");
    EXPECT_TRUE(env.exists("sub/file.txt"));
    EXPECT_EQ(env.read_file("sub/file.txt"), "hello");
}

TEST(IsolatedTestEnvTest, MkdirAll) {
    IsolatedTestEnv env;
    env.mkdir_all("a/b/c");
    EXPECT_TRUE(std::filesystem::is_directory(env.temp_dir() / "a/b/c"));
}

TEST(IsolatedTestEnvTest, SetGitignore) {
    IsolatedTestEnv env({"node_modules/", "*.log"});
    EXPECT_TRUE(env.exists(".gitignore"));
    auto content = env.read_file(".gitignore");
    EXPECT_NE(content.find("node_modules/"), std::string::npos);
    EXPECT_NE(content.find("*.log"), std::string::npos);
}

TEST(IsolatedTestEnvTest, ListFiles) {
    IsolatedTestEnv env;
    env.write_file("a.txt", "a");
    env.write_file("dir/b.txt", "b");
    auto files = env.list_files();
    EXPECT_GE(files.size(), 2u);
}

// ---------------------------------------------------------------------------
// ConcurrentHelpers
// ---------------------------------------------------------------------------
TEST(ConcurrentHelpersTest, RunConcurrentTestBasic) {
    std::atomic<int> counter{0};
    ConcurrentTestScenario scenario;
    scenario.name = "BasicCounter";
    scenario.num_threads = 4;
    scenario.ops_per_thread = 100;
    scenario.operation = [&](int, int) -> std::string {
        counter.fetch_add(1, std::memory_order_relaxed);
        return "";
    };

    auto results = RunConcurrentTest(scenario);
    EXPECT_EQ(results.total_ops, 400);
    EXPECT_EQ(results.successful_ops, 400);
    EXPECT_EQ(results.failed_ops, 0);
    EXPECT_EQ(counter.load(), 400);
}

TEST(ConcurrentHelpersTest, RunConcurrentTestWithFailures) {
    ConcurrentTestScenario scenario;
    scenario.name = "PartialFailure";
    scenario.num_threads = 2;
    scenario.ops_per_thread = 10;
    scenario.operation = [](int, int op_id) -> std::string {
        if (op_id % 3 == 0) return "simulated error";
        return "";
    };

    auto results = RunConcurrentTest(scenario);
    EXPECT_EQ(results.total_ops, 20);
    EXPECT_GT(results.failed_ops, 0);
    EXPECT_GT(results.successful_ops, 0);
}

TEST(ConcurrentHelpersTest, AssertConcurrentResultsPasses) {
    ConcurrentTestScenario scenario;
    scenario.name = "AllSuccess";
    scenario.num_threads = 2;
    scenario.ops_per_thread = 5;
    scenario.operation = [](int, int) -> std::string { return ""; };

    auto results = RunConcurrentTest(scenario);
    AssertConcurrentResults(results, 1.0);
}

TEST(ConcurrentHelpersTest, RunStressTestReturnsPositiveCount) {
    auto count = RunStressTest(
        std::chrono::milliseconds(50), 2,
        [] { return true; });
    EXPECT_GT(count, 0);
}

// ---------------------------------------------------------------------------
// PerformanceGuards
// ---------------------------------------------------------------------------
TEST(PerformanceGuardTest, MeasureAndCheck) {
    PerformanceGuard guard;
    guard.add_threshold("fast_op", {
        .max_duration = std::chrono::milliseconds(100),
    });

    guard.measure_n("fast_op", 10, [] {
        volatile int sum = 0;
        for (int i = 0; i < 1000; ++i) sum += i;
    });

    auto result = guard.check("fast_op");
    EXPECT_TRUE(result.passed)
        << "Expected fast_op to pass performance check";
    EXPECT_TRUE(result.violations.empty());
}

TEST(PerformanceGuardTest, DetectsSlowOperation) {
    PerformanceGuard guard;
    guard.add_threshold("slow_op", {
        .max_duration = std::chrono::microseconds(1),
    });

    guard.measure_n("slow_op", 5, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    auto result = guard.check("slow_op");
    EXPECT_FALSE(result.passed);
    EXPECT_FALSE(result.violations.empty());
}

TEST(PerformanceGuardTest, NoThresholdAlwaysPasses) {
    PerformanceGuard guard;
    guard.measure("untracked", [] {});
    auto result = guard.check("untracked");
    EXPECT_TRUE(result.passed);
}

TEST(PerformanceScalerTest, BasicScaling) {
    PerformanceScaler scaler;
    EXPECT_GT(scaler.cpu_count, 0);
    EXPECT_GE(scaler.scale_duration(1.0), 1.0);
    EXPECT_GT(scaler.scale_iterations(100), 0);
}

TEST(ThroughputGuardTest, MeasuresOps) {
    ThroughputGuard guard(1.0);
    auto throughput = guard.measure_ops(1000, [] {
        volatile int sum = 0;
        for (int i = 0; i < 1000; ++i) sum += i;
    });
    EXPECT_GT(throughput, 0.0);
    EXPECT_GT(guard.average_throughput(), 0.0);
}

}  // namespace
}  // namespace testing
}  // namespace lci
