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

std::vector<FileID> FileService::batch_load_from_disk(
    const std::vector<std::string>& paths) {
    if (paths.empty()) return {};

    // mmap all viable files first. MappedFile holds an mmap'd region so
    // string_views into mapped.view() stay valid until MappedFile is
    // destroyed — keep the MappedFile vector alive across the batch
    // store call.
    std::vector<MappedFile> mapped(paths.size());
    std::vector<std::pair<std::string, std::string_view>> batch;
    batch.reserve(paths.size());
    std::vector<size_t> kept_index;
    kept_index.reserve(paths.size());

    for (size_t i = 0; i < paths.size(); ++i) {
        std::string err;
        if (!mapped[i].open(paths[i], &err)) continue;
        auto view = mapped[i].view();
        if (static_cast<int64_t>(view.size()) > max_file_size_bytes_) continue;
        batch.emplace_back(paths[i], view);
        kept_index.push_back(i);
    }

    auto ids = store_->batch_load_files(batch);

    std::vector<FileID> result(paths.size(), FileID{0});
    for (size_t k = 0; k < ids.size() && k < kept_index.size(); ++k) {
        result[kept_index[k]] = ids[k];
    }
    return result;
}

std::string_view FileService::get_content(FileID id) const {
    return store_->get_content(id);
}

std::string_view FileService::get_line_content(FileID id, int line) const {
    // Delegate to the store so the returned view is pinned in the thread-local
    // slot; computing it here from a local get_file() shared_ptr would dangle
    // the view once that local pointer drops at return.
    return store_->get_line_view(id, line);
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
