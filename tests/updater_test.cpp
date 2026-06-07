#include <gtest/gtest.h>

#include <lci/update/updater.h>

using lci::update::Arch;
using lci::update::Asset;
using lci::update::Os;
using lci::update::Platform;
using lci::update::select_asset;

namespace {

// A realistic asset list as published by the release workflow.
std::vector<Asset> sample_assets() {
    return {
        {"lci-0.5.0-Linux.tar.gz", "https://example/lin.tgz"},
        {"lci-0.5.0-Linux.deb", "https://example/lin.deb"},
        {"lci-0.5.0-Linux.rpm", "https://example/lin.rpm"},
        {"lci-0.5.0-Darwin.tar.gz", "https://example/mac.tgz"},
        {"lci-0.5.0-win64.tar.gz", "https://example/win.tgz"},
    };
}

}  // namespace

TEST(UpdaterAssetSelect, LinuxX86PicksLinuxTarball) {
    auto a = select_asset(sample_assets(), Platform{Os::Linux, Arch::X86_64});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->name, "lci-0.5.0-Linux.tar.gz");
}

TEST(UpdaterAssetSelect, LinuxArm64Unsupported) {
    auto a = select_asset(sample_assets(), Platform{Os::Linux, Arch::Arm64});
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterAssetSelect, MacosX86PicksUniversalDarwin) {
    auto a = select_asset(sample_assets(), Platform{Os::Macos, Arch::X86_64});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->name, "lci-0.5.0-Darwin.tar.gz");
}

TEST(UpdaterAssetSelect, MacosArm64ServedByUniversalDarwin) {
    auto a = select_asset(sample_assets(), Platform{Os::Macos, Arch::Arm64});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->name, "lci-0.5.0-Darwin.tar.gz");
}

TEST(UpdaterAssetSelect, WindowsX86PicksWin64Tarball) {
    auto a = select_asset(sample_assets(), Platform{Os::Windows, Arch::X86_64});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->name, "lci-0.5.0-win64.tar.gz");
}

TEST(UpdaterAssetSelect, WindowsMatchesWindowsNamedTarball) {
    // CPack may emit "Windows" instead of "win64"; both must match.
    std::vector<Asset> assets = {
        {"lci-0.5.0-Windows.tar.gz", "https://example/win.tgz"},
    };
    auto a = select_asset(assets, Platform{Os::Windows, Arch::X86_64});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->name, "lci-0.5.0-Windows.tar.gz");
}

TEST(UpdaterAssetSelect, UnsupportedOsReturnsNullopt) {
    auto a = select_asset(sample_assets(),
                          Platform{Os::Unsupported, Arch::X86_64});
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterAssetSelect, NoMatchingAssetReturnsNullopt) {
    std::vector<Asset> assets = {
        {"lci-0.5.0-Darwin.tar.gz", "https://example/mac.tgz"},
    };
    auto a = select_asset(assets, Platform{Os::Linux, Arch::X86_64});
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterAssetSelect, DoesNotPickDebOrRpmForLinux) {
    // Only the tarball is a valid self-update artifact.
    std::vector<Asset> assets = {
        {"lci-0.5.0-Linux.deb", "https://example/lin.deb"},
        {"lci-0.5.0-Linux.rpm", "https://example/lin.rpm"},
    };
    auto a = select_asset(assets, Platform{Os::Linux, Arch::X86_64});
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterDetect, DetectPlatformIsConsistent) {
    Platform p = lci::update::detect_platform();
    // On the CI/dev host this must resolve to a real, supported triple.
    EXPECT_NE(p.os, Os::Unsupported);
}
