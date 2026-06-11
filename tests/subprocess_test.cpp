#include <gtest/gtest.h>

#include <lci/core/subprocess.h>

#include <filesystem>

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

}  // namespace
}  // namespace lci
