#include <gtest/gtest.h>

#include <lci/core/portable.h>
#include <lci/core/text.h>

#include <ctime>
#include <filesystem>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace lci {

TEST(TextTest, AsciiLowerIsLocaleIndependent) {
    EXPECT_EQ(text::ascii_lower("HTTP_Server"), "http_server");
    EXPECT_EQ(text::ascii_lower("already lower"), "already lower");
}

TEST(TextTest, AsciiContainsCaseInsensitive) {
    EXPECT_TRUE(text::ascii_contains_ci("Path/To/Handler.cpp", "HANDLER"));
    EXPECT_TRUE(text::ascii_contains_ci("anything", ""));
    EXPECT_FALSE(text::ascii_contains_ci("short", "longer"));
}
namespace {

TEST(PortableTest, GmtimeUtcMatchesKnownEpoch) {
    std::tm tm{};
    ASSERT_TRUE(portable::gmtime_utc(0, tm));
    EXPECT_EQ(tm.tm_year, 70);
    EXPECT_EQ(tm.tm_mon, 0);
    EXPECT_EQ(tm.tm_mday, 1);
    EXPECT_EQ(tm.tm_hour, 0);
}

TEST(PortableTest, LocaltimeLocalProducesValidTm) {
    std::tm tm{};
    ASSERT_TRUE(portable::localtime_local(std::time(nullptr), tm));
    EXPECT_GE(tm.tm_year, 126);  // >= 2026
    EXPECT_LT(tm.tm_hour, 24);
    EXPECT_LT(tm.tm_min, 60);
    EXPECT_LE(tm.tm_sec, 61);    // allow leap seconds
}

#ifndef _WIN32
TEST(PortableTest, AtomicWriteHonoursTheUmask) {
    // write_file_atomically fchmod'ed the requested mode, so a user running
    // with umask 077 got group- and world-readable 0644 context manifests.
    // The mode is a request masked by the umask, as with open(2).
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() /
        ("lci-umask-" + std::to_string(portable::process_id()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const mode_t old = ::umask(077);
    const std::string err =
        portable::write_file_atomically(dir / "m.json", "{}", 0644);
    ::umask(old);
    ASSERT_EQ(err, "");
    struct stat st{};
    ASSERT_EQ(::stat((dir / "m.json").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
    fs::remove_all(dir);
}
#endif

TEST(PortableTest, ProcessIdIsPositiveAndStable) {
    int pid = portable::process_id();
    EXPECT_GT(pid, 0);
    EXPECT_EQ(pid, portable::process_id());
}

TEST(PortableTest, ExecutablePathPointsAtTestBinary) {
    auto p = portable::executable_path();
    EXPECT_TRUE(std::filesystem::exists(p));
    EXPECT_NE(p.filename().string().find("lci_tests"), std::string::npos);
}

TEST(PortableTest, ParseDoubleBasic) {
    double v = 0;
    EXPECT_TRUE(portable::parse_double("0.85", v));
    EXPECT_DOUBLE_EQ(v, 0.85);
    EXPECT_TRUE(portable::parse_double("3", v));
    EXPECT_DOUBLE_EQ(v, 3.0);
}

TEST(PortableTest, ParseDoubleRejectsGarbage) {
    double v = 42.0;
    EXPECT_FALSE(portable::parse_double("", v));
    EXPECT_FALSE(portable::parse_double("abc", v));
}

TEST(PortableTest, ParseDoubleAcceptsNumericPrefix) {
    // Mirrors std::from_chars semantics relied on by semantic_annotator:
    // a valid numeric prefix parses successfully.
    double v = 0;
    EXPECT_TRUE(portable::parse_double("1.5x", v));
    EXPECT_DOUBLE_EQ(v, 1.5);
}

}  // namespace
}  // namespace lci
