#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace lci {
namespace testing {

namespace fs = std::filesystem;

// IsolatedTestEnv provides a temporary directory for integration testing.
// Automatically cleans up on destruction.
class IsolatedTestEnv {
  public:
    explicit IsolatedTestEnv(
        std::vector<std::string> gitignore_patterns = {})
        : temp_dir_(fs::temp_directory_path() /
                    ("lci_test_" + random_suffix())) {
        fs::create_directories(temp_dir_);
        if (!gitignore_patterns.empty()) {
            set_gitignore(gitignore_patterns);
        }
    }

    ~IsolatedTestEnv() {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    IsolatedTestEnv(const IsolatedTestEnv&) = delete;
    IsolatedTestEnv& operator=(const IsolatedTestEnv&) = delete;
    IsolatedTestEnv(IsolatedTestEnv&&) = default;
    IsolatedTestEnv& operator=(IsolatedTestEnv&&) = default;

    [[nodiscard]] const fs::path& temp_dir() const { return temp_dir_; }

    void write_file(std::string_view relative_path,
                    std::string_view content) {
        auto full = temp_dir_ / relative_path;
        fs::create_directories(full.parent_path());
        std::ofstream ofs(full, std::ios::binary);
        ASSERT_TRUE(ofs.good())
            << "Failed to create file: " << full.string();
        ofs.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
    }

    [[nodiscard]] std::string read_file(
        std::string_view relative_path) const {
        auto full = temp_dir_ / relative_path;
        std::ifstream ifs(full, std::ios::binary);
        EXPECT_TRUE(ifs.good())
            << "Failed to read file: " << full.string();
        return {std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>()};
    }

    void mkdir_all(std::string_view relative_path) {
        fs::create_directories(temp_dir_ / relative_path);
    }

    [[nodiscard]] bool exists(std::string_view relative_path) const {
        return fs::exists(temp_dir_ / relative_path);
    }

    void set_gitignore(const std::vector<std::string>& patterns) {
        std::string content;
        for (const auto& p : patterns) {
            content += p;
            content += '\n';
        }
        write_file(".gitignore", content);
    }

    [[nodiscard]] std::vector<std::string> list_files() const {
        std::vector<std::string> result;
        for (const auto& entry :
             fs::recursive_directory_iterator(temp_dir_)) {
            auto rel = fs::relative(entry.path(), temp_dir_);
            result.push_back(rel.generic_string());
        }
        return result;
    }

  private:
    fs::path temp_dir_;

    static std::string random_suffix() {
        static std::atomic<uint64_t> counter{0};
        auto pid = static_cast<uint64_t>(
#ifdef _WIN32
            _getpid()
#else
            getpid()
#endif
        );
        return std::to_string(pid) + "_" +
               std::to_string(counter.fetch_add(1));
    }
};

}  // namespace testing
}  // namespace lci
