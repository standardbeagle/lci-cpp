#include <lci/core/file_content_store.h>
#include <lci/core/mmap.h>
#include <lci/error.h>

#include <fstream>
#include <string>

namespace lci {

/// Loads a file via memory-mapping and stores its content.
/// Falls back to standard file I/O if mmap fails (e.g., /proc files).
/// Returns the FileID on success, or an Error.
Result<FileID> load_file_mmap(FileContentStore& store, const std::string& path) {
    MappedFile mapped;
    std::string mmap_error;

    if (mapped.open(path, &mmap_error)) {
        auto view = mapped.view();
        FileID id = store.load_file(path, view);
        return id;
    }

    // Fallback to standard I/O for files that cannot be mmap'd.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return make_file_error("load", path, "failed to open file");
    }

    auto size = file.tellg();
    if (size < 0) {
        return make_file_error("load", path, "failed to determine file size");
    }

    file.seekg(0, std::ios::beg);
    std::string content(static_cast<size_t>(size), '\0');
    if (!file.read(content.data(), size)) {
        return make_file_error("load", path, "failed to read file");
    }

    FileID id = store.load_file(path, content);
    return id;
}

/// Loads multiple files via memory-mapping in batch.
std::vector<Result<FileID>> batch_load_files_mmap(FileContentStore& store,
                                                   const std::vector<std::string>& paths) {
    std::vector<Result<FileID>> results;
    results.reserve(paths.size());

    for (const auto& path : paths) {
        results.push_back(load_file_mmap(store, path));
    }

    return results;
}

}  // namespace lci
