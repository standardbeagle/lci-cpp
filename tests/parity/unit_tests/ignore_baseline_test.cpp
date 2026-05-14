// tests/parity/unit_tests/ignore_baseline_test.cpp
//
// Guards the BASE_IGNORE_NON_DETERMINISM contract (DART-LaID7inbumds):
//
//   1. No descriptor redundantly lists a BASE field in its tiers.ignore.
//      Redundant entries defeat the point of centralization — when a new
//      non-determinism field is added, you'd still have to update every
//      descriptor.
//
//   2. Every descriptor whose tiers.ignore is non-empty must carry a
//      `_rationale_ignore` sibling explaining *why* those descriptor-
//      specific fields are dropped.  Catches the case where refactoring
//      deletes the rationale alongside the BASE entries.
//
// The test discovers descriptors at runtime via PARITY_DESCRIPTORS_DIR
// (set by CMake), so adding a new descriptor automatically extends the
// coverage without test changes.
//
// Diagnostic: on failure, names the descriptor file AND the specific
// field that violates the contract — no need to grep manually.

#include "runner/ignore_baseline.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<fs::path> collect_descriptors() {
    const char* root = std::getenv("PARITY_DESCRIPTORS_DIR");
    if (!root) {
        // Fall back to source-relative path so the test is runnable
        // standalone if the CMake env wasn't plumbed through.
        return {};
    }
    std::vector<fs::path> out;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == ".json" &&
            e.path().string().find(".parity.json") != std::string::npos) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST(IgnoreBaseline, BaseSetIsNonEmptyAndUnique) {
    using namespace lci::parity;
    std::set<std::string_view> seen;
    for (auto p : BASE_IGNORE_NON_DETERMINISM) {
        EXPECT_TRUE(seen.insert(p).second)
            << "duplicate entry in BASE_IGNORE_NON_DETERMINISM: " << p;
    }
    EXPECT_FALSE(seen.empty());
}

TEST(IgnoreBaseline, MergedIgnoreContainsBaseAndExtras) {
    using namespace lci::parity;
    std::vector<std::string> extras = {"foo", "bar"};
    auto merged = merged_ignore(extras);
    // BASE comes first, then extras
    ASSERT_EQ(merged.size(), BASE_IGNORE_NON_DETERMINISM.size() + 2);
    EXPECT_EQ(merged.back(), "bar");
    EXPECT_EQ(merged[merged.size() - 2], "foo");
    EXPECT_TRUE(is_base_ignore(merged.front()));
}

TEST(IgnoreBaseline, MergedIgnoreDedupesBaseOverlap) {
    using namespace lci::parity;
    // If a (stale) descriptor still lists BASE entries, merge must
    // dedupe so canonicalize doesn't get duplicates.
    std::vector<std::string> stale = {"pid", "timestamp", "custom_extra"};
    auto merged = merged_ignore(stale);
    EXPECT_EQ(merged.size(), BASE_IGNORE_NON_DETERMINISM.size() + 1);
    EXPECT_EQ(merged.back(), "custom_extra");
}

TEST(IgnoreBaseline, NoDescriptorRedundantlyListsBaseFields) {
    auto descriptors = collect_descriptors();
    ASSERT_FALSE(descriptors.empty())
        << "no descriptors discovered — PARITY_DESCRIPTORS_DIR not set?";

    std::vector<std::string> violations;
    for (const auto& path : descriptors) {
        std::string text = slurp(path);
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            violations.push_back(path.string() + ": JSON parse error: " + e.what());
            continue;
        }
        if (!j.contains("tiers") || !j["tiers"].is_object()) continue;
        const auto& tiers = j["tiers"];
        if (!tiers.contains("ignore") || !tiers["ignore"].is_array()) continue;
        for (const auto& entry : tiers["ignore"]) {
            if (!entry.is_string()) continue;
            std::string field = entry.get<std::string>();
            if (lci::parity::is_base_ignore(field)) {
                violations.push_back(
                    path.string() + ": redundantly lists BASE field '" + field +
                    "' (covered by BASE_IGNORE_NON_DETERMINISM)");
            }
        }
    }
    EXPECT_TRUE(violations.empty())
        << "found " << violations.size()
        << " BASE-overlap violation(s):\n  " <<
        [&]() {
            std::string s;
            for (auto& v : violations) { s += v; s += "\n  "; }
            return s;
        }();
}

TEST(IgnoreBaseline, NonEmptyIgnoreHasRationale) {
    auto descriptors = collect_descriptors();
    ASSERT_FALSE(descriptors.empty());

    std::vector<std::string> violations;
    for (const auto& path : descriptors) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(slurp(path));
        } catch (...) {
            continue; // parse error caught by other test
        }
        if (!j.contains("tiers") || !j["tiers"].is_object()) continue;
        const auto& tiers = j["tiers"];
        if (!tiers.contains("ignore") || !tiers["ignore"].is_array()) continue;
        if (tiers["ignore"].empty()) continue;
        // Non-empty descriptor-specific ignore — must explain WHY.
        if (!tiers.contains("_rationale_ignore") ||
            !tiers["_rationale_ignore"].is_string() ||
            tiers["_rationale_ignore"].get<std::string>().empty()) {
            violations.push_back(
                path.string() +
                ": non-empty tiers.ignore lacks `_rationale_ignore` "
                "(BASE_IGNORE_NON_DETERMINISM applies implicitly; "
                "remaining entries must be justified)");
        }
    }
    EXPECT_TRUE(violations.empty())
        << "found " << violations.size()
        << " missing-rationale violation(s):\n  " <<
        [&]() {
            std::string s;
            for (auto& v : violations) { s += v; s += "\n  "; }
            return s;
        }();
}
