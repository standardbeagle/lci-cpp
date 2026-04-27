#include <lci/core/file_service.h>
#include <lci/core/mmap.h>

namespace lci {

FileService::FileService(std::shared_ptr<FileContentStore> store,
                         int64_t max_file_size_bytes)
    : store_(store ? std::move(store) : std::make_shared<FileContentStore>()),
      max_file_size_bytes_(max_file_size_bytes) {}

Result<FileID> FileService::load_file(const std::string& path,
                                       std::string_view content) {
    if (static_cast<int64_t>(content.size()) > max_file_size_bytes_) {
        Error e;
        e.type = ErrorType::FileTooLarge;
        e.file_path = path;
        e.message = "file too large (" + std::to_string(content.size()) +
                    " bytes, limit " + std::to_string(max_file_size_bytes_) + ")";
        e.operation = "load";
        return e;
    }

    return store_->load_file(path, content);
}

Result<FileID> FileService::load_file_from_disk(const std::string& path) {
    MappedFile mapped;
    std::string mmap_error;

    if (!mapped.open(path, &mmap_error)) {
        return make_file_error("load", path, "failed to open file: " + mmap_error);
    }

    auto view = mapped.view();
    if (static_cast<int64_t>(view.size()) > max_file_size_bytes_) {
        Error e;
        e.type = ErrorType::FileTooLarge;
        e.file_path = path;
        e.message = "file too large (" + std::to_string(view.size()) +
                    " bytes, limit " + std::to_string(max_file_size_bytes_) + ")";
        e.operation = "load";
        return e;
    }

    return store_->load_file(path, view);
}

std::string_view FileService::get_content(FileID id) const {
    return store_->get_content(id);
}

std::string_view FileService::get_line_content(FileID id, int line) const {
    auto fc = store_->get_file(id);
    if (!fc) return {};

    const auto& offsets = fc->line_offsets;
    if (line < 0 || line >= static_cast<int>(offsets.size())) return {};

    auto content = fc->view();
    uint32_t start = offsets[static_cast<size_t>(line)];
    uint32_t end;

    if (line + 1 < static_cast<int>(offsets.size())) {
        end = offsets[static_cast<size_t>(line + 1)];
        if (end > start && content[end - 1] == '\n') {
            --end;
        }
    } else {
        end = static_cast<uint32_t>(content.size());
    }

    // Strip trailing \r for CRLF normalization.
    if (end > start && content[end - 1] == '\r') {
        --end;
    }

    return content.substr(start, end - start);
}

int FileService::get_line_count(FileID id) const {
    return store_->get_line_count(id);
}

FileContentStore& FileService::store() {
    return *store_;
}

const FileContentStore& FileService::store() const {
    return *store_;
}

int64_t FileService::max_file_size_bytes() const {
    return max_file_size_bytes_;
}

}  // namespace lci
