#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "runner/modes/http.h"

namespace lci::parity {
namespace {

namespace fs = std::filesystem;

// SUNSET-WHEN-GO-UPGRADES (Dart task BNXsh3tUpMSW): when the Go
// reference binary adopts the uid-namespaced socket path, drop the
// second candidate from candidate_socket_paths() and update the
// expectation counts here to 1.

TEST(SocketPath, ReturnsTwoCandidatesWhileSunsetPending) {
    auto cands = candidate_socket_paths_for_test("/tmp/some/corpus");
    EXPECT_EQ(cands.size(), 2u)
        << "If this is 1, the Go reference has been upgraded — drop the "
           "second candidate in tests/parity/runner/modes/http.cpp and "
           "update this test to expect size==1.";
}

TEST(SocketPath, FirstCandidateIsUidNamespacedNewFormat) {
    auto cands = candidate_socket_paths_for_test("/tmp/some/corpus");
    ASSERT_GE(cands.size(), 1u);
    fs::path p(cands[0]);
    std::string name = p.filename().string();
    // Format: lci-<uid>-<8hex>.sock
    EXPECT_TRUE(name.starts_with("lci-")) << name;
    EXPECT_TRUE(name.ends_with(".sock")) << name;
    EXPECT_EQ(p.parent_path(), fs::temp_directory_path());

    // Must contain the literal uid as a decimal-number segment.
    auto uid_str = std::to_string(static_cast<unsigned>(::getuid()));
    EXPECT_NE(name.find("-" + uid_str + "-"), std::string::npos)
        << "First candidate must include the current uid: " << name;
}

TEST(SocketPath, SecondCandidateIsLegacyGoFormat) {
    auto cands = candidate_socket_paths_for_test("/tmp/some/corpus");
    ASSERT_GE(cands.size(), 2u);
    fs::path p(cands[1]);
    std::string name = p.filename().string();
    EXPECT_TRUE(name.starts_with("lci-server-"));
    EXPECT_TRUE(name.ends_with(".sock"));
    EXPECT_EQ(p.parent_path(), fs::temp_directory_path());
    // Legacy format must NOT contain the uid.
    auto uid_str = std::to_string(static_cast<unsigned>(::getuid()));
    EXPECT_EQ(name.find("-" + uid_str + "-"), std::string::npos)
        << "Legacy lci-server-<hash>.sock must not embed uid: " << name;
}

TEST(SocketPath, DifferentCorporaProduceDifferentPaths) {
    auto a = candidate_socket_paths_for_test("/tmp/corpus/alpha");
    auto b = candidate_socket_paths_for_test("/tmp/corpus/beta");
    ASSERT_GE(a.size(), 1u);
    ASSERT_GE(b.size(), 1u);
    EXPECT_NE(a[0], b[0]) << "Different roots must hash to different sockets";
    EXPECT_NE(a[1], b[1]);
}

TEST(SocketPath, SameCorpusProducesStablePaths) {
    auto a = candidate_socket_paths_for_test("/tmp/corpus/x");
    auto b = candidate_socket_paths_for_test("/tmp/corpus/x");
    ASSERT_EQ(a, b) << "Socket path must be a pure function of corpus path";
}

TEST(SocketPath, EachCandidateUsesSunPathSafeLength) {
    // Unix sun_path is 108 bytes on Linux. Ensure all candidates fit.
    auto cands = candidate_socket_paths_for_test("/tmp/some/long/corpus/path");
    for (const auto& p : cands) {
        EXPECT_LT(p.size(), 108u) << p;
    }
}

}  // namespace
}  // namespace lci::parity
