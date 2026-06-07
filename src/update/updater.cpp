#include <lci/update/updater.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace lci::update {
namespace fs = std::filesystem;

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    auto lower = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    };
    std::string h;
    h.reserve(haystack.size());
    for (char c : haystack) h.push_back(lower(static_cast<unsigned char>(c)));
    std::string n;
    n.reserve(needle.size());
    for (char c : needle) n.push_back(lower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// Strip a single leading 'v' so "v0.5.0" and "0.5.0" compare equal.
std::string strip_v(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) return tag.substr(1);
    return tag;
}

// Run a command, capturing stdout. Returns false if the process could not be
// started or exited non-zero.
bool run_capture(const std::string& cmd, std::string& out) {
    out.clear();
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) return false;
    std::array<char, 4096> buf{};
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        out.append(buf.data(), n);
    }
    int rc = ::pclose(pipe);
    return rc == 0;
}

// Run a command for its exit code (stdout/stderr inherit the terminal).
int run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
#if defined(_WIN32)
    return rc;
#else
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
#endif
}

bool have_tool(const std::string& name) {
    std::string out;
    return run_capture(name + " --version", out);
}

// URLs and filenames from the API response are passed to curl/tar/sha256sum
// through a shell (popen/system). They are wrapped in double quotes, so the
// only characters that remain dangerous in that context are those that survive
// double-quoting: $ ` " \ and whitespace. We additionally pin the scheme to
// https and the host to GitHub, so a compromised/MITM'd response cannot
// redirect the shell to an arbitrary command or host.
bool is_github_host(const std::string& host) {
    return host == "github.com" || host == "api.github.com" ||
           host == "codeload.github.com" || host == "githubusercontent.com" ||
           ends_with(host, ".githubusercontent.com");
}

// A tag/version is embedded into a shell URL; restrict it to a safe charset.
bool safe_version(const std::string& v) {
    if (v.empty()) return false;
    for (char c : v) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

// Compute the SHA-256 of a file by shelling out to the platform tool — the
// same shell-out strategy used for curl/tar. Returns lowercase hex, or empty
// on failure (no tool / parse failure).
std::string sha256_of_file(const fs::path& f) {
    std::string out;
#if defined(_WIN32)
    if (!run_capture("certutil -hashfile \"" + f.string() + "\" SHA256", out)) {
        return {};
    }
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        std::string h;
        for (char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                h.push_back(static_cast<char>(std::tolower((unsigned char)c)));
            }
        }
        if (is_hex64(h)) return h;
    }
    return {};
#else
    if (run_capture("sha256sum \"" + f.string() + "\"", out) ||
        run_capture("shasum -a 256 \"" + f.string() + "\"", out)) {
        std::string tok = to_lower(out.substr(0, out.find_first_of(" \t\n")));
        if (is_hex64(tok)) return tok;
    }
    return {};
#endif
}

fs::path current_executable() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(),
                                   static_cast<DWORD>(buf.size()));
    while (len == buf.size()) {  // truncated — grow and retry
        buf.resize(buf.size() * 2);
        len = GetModuleFileNameW(nullptr, buf.data(),
                                 static_cast<DWORD>(buf.size()));
    }
    buf.resize(len);
    return fs::path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    return fs::weakly_canonical(fs::path(buf.c_str()));
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p;
#endif
}

// Locate the extracted binary (lci / lci.exe) under a directory tree.
fs::path find_extracted_binary(const fs::path& root) {
#if defined(_WIN32)
    const std::string want = "lci.exe";
#else
    const std::string want = "lci";
#endif
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().filename() == want) {
            return it->path();
        }
    }
    return {};
}

// Replace the running executable with `new_bin`. Fails fast (returns false +
// stderr message) when the install directory is not writable.
bool replace_self(const fs::path& self, const fs::path& new_bin) {
    fs::path dir = self.parent_path();

#if !defined(_WIN32)
    if (::access(dir.c_str(), W_OK) != 0) {
        std::cerr << "Error: no write permission to " << dir << "\n"
                  << "Re-run with elevated privileges: sudo lci update\n";
        return false;
    }
    fs::path staged = dir / ".lci.update.new";
    std::error_code ec;
    fs::remove(staged, ec);
    fs::copy_file(new_bin, staged, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error: failed to stage new binary: " << ec.message()
                  << "\n";
        return false;
    }
    fs::permissions(staged,
                    fs::perms::owner_all | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    // rename(2) atomically replaces the running binary on POSIX.
    if (::rename(staged.c_str(), self.c_str()) != 0) {
        fs::remove(staged, ec);
        std::cerr << "Error: failed to replace " << self
                  << " (permission denied?). Try: sudo lci update\n";
        return false;
    }
    return true;
#else
    // Windows cannot overwrite a running .exe: move it aside, then move the new
    // one in. The stale .old is cleaned up best-effort on the next run.
    std::error_code ec;
    fs::path old = self;
    old += ".old";
    fs::remove(old, ec);  // clear any leftover from a previous update
    fs::rename(self, old, ec);
    if (ec) {
        std::cerr << "Error: failed to move current binary aside: "
                  << ec.message()
                  << " (run from an Administrator shell?)\n";
        return false;
    }
    fs::copy_file(new_bin, self, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::rename(old, self, ec);  // roll back
        std::cerr << "Error: failed to install new binary: " << ec.message()
                  << "\n";
        return false;
    }
    return true;
#endif
}

}  // namespace

Platform detect_platform() {
    Platform p{};
#if defined(_WIN32)
    p.os = Os::Windows;
#elif defined(__APPLE__)
    p.os = Os::Macos;
#elif defined(__linux__)
    p.os = Os::Linux;
#else
    p.os = Os::Unsupported;
#endif

#if defined(__x86_64__) || defined(_M_X64)
    p.arch = Arch::X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    p.arch = Arch::Arm64;
#else
    p.arch = Arch::Other;
#endif
    return p;
}

std::optional<Asset> select_asset(const std::vector<Asset>& assets,
                                  Platform p) {
    auto pick = [&](auto match) -> std::optional<Asset> {
        for (const auto& a : assets) {
            if (match(a.name)) return a;
        }
        return std::nullopt;
    };

    switch (p.os) {
        case Os::Linux:
            if (p.arch != Arch::X86_64) return std::nullopt;
            return pick([](const std::string& n) {
                return ends_with(n, "Linux.tar.gz");
            });
        case Os::Macos:
            // Universal binary serves both x86_64 and arm64.
            return pick([](const std::string& n) {
                return ends_with(n, "Darwin.tar.gz");
            });
        case Os::Windows:
            if (p.arch != Arch::X86_64) return std::nullopt;
            return pick([](const std::string& n) {
                return ends_with(n, ".tar.gz") &&
                       (contains_ci(n, "win64") || contains_ci(n, "windows"));
            });
        case Os::Unsupported:
        default:
            return std::nullopt;
    }
}

bool is_safe_download_url(const std::string& url) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    // Reject anything that could break out of the double-quoted shell argument.
    for (char c : url) {
        if (c == '$' || c == '`' || c == '"' || c == '\\' || c == ' ' ||
            c == '\t' || c == '\n' || c == '\r') {
            return false;
        }
    }
    size_t host_start = scheme.size();
    size_t host_end = url.find('/', host_start);
    std::string host = url.substr(
        host_start, host_end == std::string::npos ? std::string::npos
                                                  : host_end - host_start);
    // Drop any port suffix before matching.
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return is_github_host(host);
}

bool is_safe_asset_name(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (char c : n) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::string expected_hash_for(const std::string& sums_text,
                              const std::string& asset_name) {
    std::istringstream iss(sums_text);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim trailing CR/whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
            line.pop_back();
        }
        // Split "<hash><spaces><filename>" and compare the filename exactly —
        // a suffix match would let "extra-foo.tar.gz" satisfy "foo.tar.gz".
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        size_t fn = line.find_first_not_of(" \t", sp);
        if (fn == std::string::npos) continue;
        if (line.compare(fn, std::string::npos, asset_name) != 0) continue;
        std::string tok = to_lower(line.substr(0, sp));
        if (is_hex64(tok)) return tok;
    }
    return {};
}

int run_update(const UpdateConfig& cfg) {
    Platform plat = detect_platform();
    if (plat.os == Os::Unsupported || plat.arch == Arch::Other ||
        (plat.os == Os::Linux && plat.arch != Arch::X86_64)) {
        std::cerr << "Error: self-update is not available for this platform "
                     "(no matching release artifact).\n"
                     "Build from source: "
                     "https://github.com/"
                  << cfg.repo << "#from-source\n";
        return 1;
    }

    if (!have_tool("curl")) {
        std::cerr << "Error: `curl` is required for self-update but was not "
                     "found on PATH.\n";
        return 1;
    }
    if (!cfg.check_only && !have_tool("tar")) {
        std::cerr << "Error: `tar` is required for self-update but was not "
                     "found on PATH.\n";
        return 1;
    }

    if (!cfg.target_version.empty() && !safe_version(cfg.target_version)) {
        std::cerr << "Error: invalid --version '" << cfg.target_version
                  << "' (allowed: letters, digits, '.', '_', '-').\n";
        return 1;
    }

    std::string api_url = "https://api.github.com/repos/" + cfg.repo;
    if (cfg.target_version.empty()) {
        api_url += "/releases/latest";
    } else {
        api_url += "/releases/tags/v" + cfg.target_version;
    }

    std::string json_text;
    std::string fetch =
        "curl -fsSL --proto =https -H \"Accept: application/vnd.github+json\" "
        "\"" +
        api_url + "\"";
    if (!run_capture(fetch, json_text) || json_text.empty()) {
        std::cerr << "Error: failed to query the GitHub releases API ("
                  << api_url << ").\n";
        return 1;
    }

    std::string tag;
    std::vector<Asset> assets;
    try {
        auto j = nlohmann::json::parse(json_text);
        tag = j.value("tag_name", "");
        if (j.contains("assets") && j["assets"].is_array()) {
            for (const auto& a : j["assets"]) {
                assets.push_back(Asset{a.value("name", ""),
                                       a.value("browser_download_url", "")});
            }
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Error: could not parse the releases API response: "
                  << e.what() << "\n";
        return 1;
    }

    if (tag.empty()) {
        std::cerr << "Error: no release found"
                  << (cfg.target_version.empty()
                          ? ""
                          : " for tag v" + cfg.target_version)
                  << ".\n";
        return 1;
    }

    std::string latest = strip_v(tag);
    bool up_to_date = (latest == cfg.current_version);

    if (cfg.check_only) {
        if (up_to_date) {
            std::cout << "lci is up to date (" << cfg.current_version << ").\n";
        } else {
            std::cout << "Update available: " << cfg.current_version << " -> "
                      << latest << "\nRun: lci update\n";
        }
        return 0;
    }

    if (up_to_date && !cfg.force) {
        std::cout << "lci is already up to date (" << cfg.current_version
                  << "). Use --force to reinstall.\n";
        return 0;
    }

    auto asset = select_asset(assets, plat);
    if (!asset || asset->url.empty()) {
        std::cerr << "Error: release " << tag
                  << " has no asset matching this platform.\n";
        return 1;
    }
    if (!is_safe_asset_name(asset->name) || !is_safe_download_url(asset->url)) {
        std::cerr << "Error: release asset has an unexpected name or download "
                     "URL; refusing to proceed (" << asset->name << ").\n";
        return 1;
    }

    std::error_code ec;
    fs::path work = fs::temp_directory_path(ec) /
                    ("lci-update-" + std::to_string(::getpid()));
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) {
        std::cerr << "Error: could not create temp directory " << work << ": "
                  << ec.message() << "\n";
        return 1;
    }

    fs::path tarball = work / asset->name;
    std::cout << "Downloading " << asset->name << " (" << latest << ")...\n";
    std::string dl = "curl -fsSL --proto =https -o \"" + tarball.string() +
                     "\" \"" + asset->url + "\"";
    if (run_cmd(dl) != 0) {
        std::cerr << "Error: download failed (" << asset->url << ").\n";
        fs::remove_all(work, ec);
        return 1;
    }

    // Verify integrity against the release SHA256SUMS when present.
    std::string sums_url;
    for (const auto& a : assets) {
        if (a.name == "SHA256SUMS") {
            sums_url = a.url;
            break;
        }
    }
    if (!sums_url.empty() && !is_safe_download_url(sums_url)) {
        std::cerr << "Error: SHA256SUMS has an unexpected download URL; "
                     "refusing to proceed.\n";
        fs::remove_all(work, ec);
        return 1;
    }
    if (!sums_url.empty()) {
        std::string sums_text;
        if (!run_capture("curl -fsSL --proto =https \"" + sums_url + "\"",
                         sums_text)) {
            std::cerr << "Error: failed to fetch SHA256SUMS for verification.\n";
            fs::remove_all(work, ec);
            return 1;
        }
        std::string expected = expected_hash_for(sums_text, asset->name);
        if (expected.empty()) {
            std::cerr << "Error: SHA256SUMS has no entry for " << asset->name
                      << ".\n";
            fs::remove_all(work, ec);
            return 1;
        }
        std::string actual = sha256_of_file(tarball);
        if (actual.empty()) {
            std::cerr << "Error: no sha256 tool (sha256sum/shasum/certutil) "
                         "found to verify the download.\n";
            fs::remove_all(work, ec);
            return 1;
        }
        if (actual != expected) {
            std::cerr << "Error: checksum mismatch for " << asset->name
                      << " (expected " << expected << ", got " << actual
                      << ").\n";
            fs::remove_all(work, ec);
            return 1;
        }
        std::cout << "Verified checksum.\n";
    } else {
        std::cerr << "warning: release has no SHA256SUMS; skipping integrity "
                     "check.\n";
    }

    std::string extract = "tar -xzf \"" + tarball.string() + "\" -C \"" +
                          work.string() + "\"";
    if (run_cmd(extract) != 0) {
        std::cerr << "Error: failed to extract " << asset->name << ".\n";
        fs::remove_all(work, ec);
        return 1;
    }

    fs::path new_bin = find_extracted_binary(work);
    if (new_bin.empty()) {
        std::cerr << "Error: extracted archive did not contain the lci "
                     "binary.\n";
        fs::remove_all(work, ec);
        return 1;
    }

    fs::path self = current_executable();
    if (self.empty()) {
        std::cerr << "Error: could not determine the path of the running "
                     "executable.\n";
        fs::remove_all(work, ec);
        return 1;
    }

    if (!replace_self(self, new_bin)) {
        fs::remove_all(work, ec);
        return 1;
    }

    fs::remove_all(work, ec);
    std::cout << "Updated lci: " << cfg.current_version << " -> " << latest
              << "\n"
              << self << "\n";
    return 0;
}

}  // namespace lci::update
