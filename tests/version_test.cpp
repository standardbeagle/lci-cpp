#include <gtest/gtest.h>

#include <lci/version.h>

TEST(VersionTest, VersionStringIsSet) {
    EXPECT_NE(lci::kVersion, nullptr);
    EXPECT_GT(std::string_view(lci::kVersion).size(), 0);
}

TEST(VersionTest, VersionComponentsAreNonNegative) {
    EXPECT_GE(lci::kVersionMajor, 0);
    EXPECT_GE(lci::kVersionMinor, 0);
    EXPECT_GE(lci::kVersionPatch, 0);
}
