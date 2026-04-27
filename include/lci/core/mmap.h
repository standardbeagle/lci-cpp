#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lci {

/// RAII wrapper for memory-mapped file data.
/// Provides zero-copy read-only access to file contents.
/// Uses mmap() on POSIX and CreateFileMapping on Windows.
class MappedFile {
  public:
    MappedFile() = default;
    ~MappedFile() { close(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : data_(other.data_), size_(other.size_)
#ifdef _WIN32
          ,
          file_handle_(other.file_handle_),
          mapping_handle_(other.mapping_handle_)
#endif
    {
        other.data_ = nullptr;
        other.size_ = 0;
#ifdef _WIN32
        other.file_handle_ = INVALID_HANDLE_VALUE;
        other.mapping_handle_ = nullptr;
#endif
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            close();
            data_ = other.data_;
            size_ = other.size_;
#ifdef _WIN32
            file_handle_ = other.file_handle_;
            mapping_handle_ = other.mapping_handle_;
            other.file_handle_ = INVALID_HANDLE_VALUE;
            other.mapping_handle_ = nullptr;
#endif
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    /// Maps a file into memory for read-only access.
    /// Returns false on failure and populates error_out if provided.
    bool open(const std::string& path, std::string* error_out = nullptr) {
        close();

#ifdef _WIN32
        file_handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_handle_ == INVALID_HANDLE_VALUE) {
            if (error_out) *error_out = "failed to open file: " + path;
            return false;
        }

        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(file_handle_, &file_size)) {
            if (error_out) *error_out = "failed to get file size: " + path;
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        if (file_size.QuadPart == 0) {
            size_ = 0;
            data_ = nullptr;
            return true;
        }

        size_ = static_cast<size_t>(file_size.QuadPart);
        mapping_handle_ = CreateFileMappingA(file_handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_handle_ == nullptr) {
            if (error_out) *error_out = "failed to create file mapping: " + path;
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        data_ = static_cast<const uint8_t*>(
            MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0));
        if (data_ == nullptr) {
            if (error_out) *error_out = "failed to map view of file: " + path;
            CloseHandle(mapping_handle_);
            mapping_handle_ = nullptr;
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (error_out) *error_out = "failed to open file: " + path;
            return false;
        }

        struct stat st {};
        if (fstat(fd, &st) != 0) {
            if (error_out) *error_out = "failed to stat file: " + path;
            ::close(fd);
            return false;
        }

        if (st.st_size == 0) {
            size_ = 0;
            data_ = nullptr;
            ::close(fd);
            return true;
        }

        size_ = static_cast<size_t>(st.st_size);
        void* mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);

        if (mapped == MAP_FAILED) {
            if (error_out) *error_out = "failed to mmap file: " + path;
            size_ = 0;
            return false;
        }

        data_ = static_cast<const uint8_t*>(mapped);
#endif
        return true;
    }

    /// Releases the mapping.
    void close() {
        if (data_ == nullptr && size_ == 0) return;

#ifdef _WIN32
        if (data_ != nullptr) UnmapViewOfFile(data_);
        if (mapping_handle_ != nullptr) CloseHandle(mapping_handle_);
        if (file_handle_ != INVALID_HANDLE_VALUE) CloseHandle(file_handle_);
        file_handle_ = INVALID_HANDLE_VALUE;
        mapping_handle_ = nullptr;
#else
        if (data_ != nullptr) {
            munmap(const_cast<uint8_t*>(data_), size_);
        }
#endif
        data_ = nullptr;
        size_ = 0;
    }

    /// Returns the mapped data pointer (null if empty or not mapped).
    const uint8_t* data() const { return data_; }

    /// Returns the file size in bytes.
    size_t size() const { return size_; }

    /// Returns true if data is currently mapped.
    bool is_open() const { return data_ != nullptr || size_ == 0; }

    /// Returns the mapped content as a string_view.
    std::string_view view() const {
        if (data_ == nullptr) return {};
        return {reinterpret_cast<const char*>(data_), size_};
    }

  private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle_ = nullptr;
#endif
};

}  // namespace lci
