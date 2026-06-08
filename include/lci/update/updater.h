#pragma once

// Self-update for the lci binary. Resolves the latest (or a pinned) GitHub
// release, downloads the platform tarball, and atomically replaces the running
// executable.
//
// Network/extract are delegated to system `curl` and `tar` — the same tools
// install.sh already requires — so the self-updater adds no new build-time
// dependency. The release JSON is parsed natively with nlohmann (no text
// scraping). See docs/superpowers/specs/2026-06-07-install-update-distribution-design.md.

#include <optional>
#include <string>
#include <vector>

namespace lci::update {

enum class Os { Linux, Macos, Windows, Unsupported };
enum class Arch { X86_64, Arm64, Other };

struct Platform {
    Os os;
    Arch arch;
};

// One downloadable asset from a GitHub release.
struct Asset {
    std::string name;  // e.g. "lci-0.5.0-Linux.tar.gz"
    std::string url;   // browser_download_url
};

// Detect the platform this binary is running on. Resolved at compile time for
// the OS; arch is read at runtime.
Platform detect_platform();

// Pure: pick the release asset matching the platform, per the contract:
//   Linux x86_64        -> /lci-.*Linux\.tar\.gz$/
//   macOS x86_64/arm64  -> /lci-.*Darwin\.tar\.gz$/   (universal binary)
//   Windows x86_64      -> /lci-.*(win64|Windows)\.tar\.gz$/i
// Returns nullopt when the platform is unsupported or no asset matches.
std::optional<Asset> select_asset(const std::vector<Asset>& assets, Platform p);

// Pure security guards: a download URL/asset name from the API response is
// passed to curl/tar via a shell, so reject anything that is not an https
// GitHub URL free of shell-breaking characters, or a plain filename.
bool is_safe_download_url(const std::string& url);
bool is_safe_asset_name(const std::string& name);

// Pure: find the expected lowercase-hex SHA-256 for `asset_name` in a
// SHA256SUMS body ("<hash>  <filename>" per line). Empty if absent or the
// matched token is not a valid 64-char hex digest.
std::string expected_hash_for(const std::string& sums_text,
                              const std::string& asset_name);

struct UpdateConfig {
    std::string repo = "standardbeagle/lci-cpp";
    std::string current_version;  // e.g. lci::kVersion (no leading 'v')
    std::string target_version;   // empty => latest; else a tag like "0.6.0"
    bool check_only = false;      // report current vs latest, do not write
    bool force = false;           // reinstall even when already current
};

// Orchestrate the update. Returns a process exit code (0 = success / nothing
// to do). Fails fast with a clear stderr message on every error path: missing
// tools, unsupported platform, no matching asset, download/extract failure, or
// no write permission to the install directory.
int run_update(const UpdateConfig& cfg);

}  // namespace lci::update
