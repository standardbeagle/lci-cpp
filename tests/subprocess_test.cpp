#include <gtest/gtest.h>

#include <lci/core/subprocess.h>

#include <chrono>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace lci {
namespace {

namespace fs = std::filesystem;

TEST(SubprocessTest, CapturesStdout) {
    std::string out;
    ASSERT_TRUE(subprocess::run_capture({"git", "--version"}, "", out));
    EXPECT_NE(out.find("git version"), std::string::npos);
}

TEST(SubprocessTest, NonZeroExitReturnsFalse) {
    std::string out;
    // `git rev-parse --show-toplevel` outside any repo exits non-zero.
    EXPECT_FALSE(subprocess::run_capture(
        {"git", "-C", fs::temp_directory_path().string(), "rev-parse",
         "--show-toplevel"},
        "", out));
}

TEST(SubprocessTest, MissingBinaryReturnsFalse) {
    std::string out;
    EXPECT_FALSE(subprocess::run_capture({"lci-no-such-binary-xyzzy"}, "", out));
}

TEST(SubprocessTest, CwdIsApplied) {
    auto tmp = fs::temp_directory_path();
    std::string out;
#if defined(_WIN32)
    ASSERT_TRUE(subprocess::run_capture({"cmd.exe", "/c", "cd"}, tmp.string(), out));
#else
    ASSERT_TRUE(subprocess::run_capture({"pwd"}, tmp.string(), out));
#endif
    // Compare canonically: /tmp may be a symlink (macOS /private/tmp).
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    EXPECT_EQ(fs::canonical(out), fs::canonical(tmp));
}

TEST(SubprocessTest, ArgsWithSpacesAndQuotesSurvive) {
    std::string out;
    // cmake -E echo round-trips its args verbatim; no shell, so no env
    // expansion and no quote stripping.
    ASSERT_TRUE(subprocess::run_capture(
        {"cmake", "-E", "echo", "a b", "c\"d", "%PATH%", "$HOME"}, "", out));
    EXPECT_NE(out.find("a b"), std::string::npos);
    EXPECT_NE(out.find("c\"d"), std::string::npos);
    EXPECT_NE(out.find("%PATH%"), std::string::npos);  // no env expansion
    EXPECT_NE(out.find("$HOME"), std::string::npos);    // no shell involved
}

TEST(SubprocessTest, RunStatusReturnsExitCode) {
    EXPECT_EQ(subprocess::run_status({"cmake", "-E", "true"}), 0);
    EXPECT_NE(subprocess::run_status({"cmake", "-E", "false"}), 0);
    EXPECT_EQ(subprocess::run_status({"lci-no-such-binary-xyzzy"}), -1);
}

TEST(SubprocessTest, SpawnDetachedReturnsTrue) {
    EXPECT_TRUE(subprocess::spawn_detached({"cmake", "-E", "true"}));
}

TEST(SubprocessTest, SpawnDetachedMissingBinaryReturnsFalse) {
    EXPECT_FALSE(subprocess::spawn_detached({"lci-no-such-binary-xyzzy"}));
}

#if !defined(_WIN32)
// The kernel's per-process fd table: /proc/self/fd on Linux, /dev/fd (fdescfs)
// on macOS, which has no procfs.
#if defined(__APPLE__)
constexpr const char* kFdTableDir = "/dev/fd";
#else
constexpr const char* kFdTableDir = "/proc/self/fd";
#endif

TEST(SubprocessTest, ConcurrentRunCaptureLeaksNoFdsIntoOtherChild) {
    // Reference value: the KERNEL's fd table for the second child
    // (/proc/self/fd), not any output of run_capture logic. While a 2s
    // sleeper's run_capture holds its pipe read end in the parent, a second
    // run_capture child must inherit nothing beyond stdio: without
    // close-on-exec pipe ends the sleeper's read end shows up as an extra
    // fd in the second child. `ls` itself holds one transient fd (the
    // directory stream), so a clean table lists exactly 0,1,2,3.
    // Baseline with no concurrent run_capture: whatever fds the test
    // harness itself inherited (ctest pipes, etc.) show up here, so the
    // comparison below isolates exactly the leak.
    auto count_fds = [] {
        std::string listing;
        EXPECT_TRUE(
            subprocess::run_capture({"ls", kFdTableDir}, "", listing));
        int entries = 0;
        for (char c : listing) {
            if (c == '\n') ++entries;
        }
        return entries;
    };
    const int baseline = count_fds();

    std::string slow_out;
    std::thread slow([&] {
        EXPECT_TRUE(subprocess::run_capture({"sleep", "2"}, "", slow_out));
    });
    // Let the sleeper's run_capture create its pipe and spawn before the
    // second run_capture starts, so its read end is open in the parent.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int concurrent = count_fds();
    slow.join();
    EXPECT_EQ(concurrent, baseline)
        << "a concurrent run_capture's pipe ends leaked into this child";
}

// run_capture children inherited the parent's stdin. Under `lci mcp` that
// is the JSON-RPC pipe: any child that reads stdin (a git credential or
// pager prompt, a hook) would swallow client frames. Reference: the child
// is `cat`, which echoes whatever stdin it was given.
TEST(SubprocessTest, RunCaptureChildGetsNoParentStdin) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const std::string frame = "{\"jsonrpc\":\"2.0\",\"id\":7}\n";
    ASSERT_EQ(::write(fds[1], frame.data(), frame.size()),
              static_cast<ssize_t>(frame.size()));
    ::close(fds[1]);  // EOF after the frame, so a pre-fix `cat` returns
    const int saved_stdin = ::dup(STDIN_FILENO);
    ASSERT_GE(saved_stdin, 0);
    ASSERT_GE(::dup2(fds[0], STDIN_FILENO), 0);
    ::close(fds[0]);

    std::string out;
    const bool ok = subprocess::run_capture({"cat"}, "", out);

    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out, "") << "the child read the parent's stdin";
}
#endif

}  // namespace
}  // namespace lci
