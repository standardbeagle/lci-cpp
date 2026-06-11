#include <lci/core/file_content_store.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <absl/container/flat_hash_set.h>
#include <xxhash.h>

namespace lci {
namespace {

/// Sentinel for "not found" lookups in snapshot.
const std::shared_ptr<FileContent> kNullContent;

/// SHA-256 round constants.
constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

/// Computes SHA-256 of a byte buffer.
std::array<uint8_t, 32> compute_sha256(std::string_view data) {
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    auto len = data.size();
    auto* src = reinterpret_cast<const uint8_t*>(data.data());

    // Pad message: append 1-bit, zeros, then 64-bit big-endian length.
    size_t padded_len = ((len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> buf(padded_len, 0);
    std::memcpy(buf.data(), src, len);
    buf[len] = 0x80;
    uint64_t bit_len = uint64_t(len) * 8;
    for (size_t i = 0; i < 8; ++i) {
        buf[padded_len - 1 - i] = uint8_t(bit_len >> (i * 8));
    }

    // Process each 64-byte block.
    for (size_t off = 0; off < padded_len; off += 64) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            w[i] = read_be32(buf.data() + off + i * 4);
        }
        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h0, b = h1, c = h2, d = h3;
        uint32_t e = h4, f = h5, g = h6, h = h7;

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + kSha256K[size_t(i)] + w[size_t(i)];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::array<uint8_t, 32> result{};
    write_be32(result.data(), h0);
    write_be32(result.data() + 4, h1);
    write_be32(result.data() + 8, h2);
    write_be32(result.data() + 12, h3);
    write_be32(result.data() + 16, h4);
    write_be32(result.data() + 20, h5);
    write_be32(result.data() + 24, h6);
    write_be32(result.data() + 28, h7);
    return result;
}

}  // namespace

// -- FileContentSnapshot ------------------------------------------------------

const std::shared_ptr<FileContent>& FileContentSnapshot::find_by_id(FileID id) const {
    auto it = id_index.find(id);
    if (it == id_index.end()) return kNullContent;
    return entries[it->second].content;
}

const std::shared_ptr<FileContent>& FileContentSnapshot::find_by_path(
    const std::string& path) const {
    auto it = path_index.find(path);
    if (it == path_index.end()) return kNullContent;
    return entries[it->second].content;
}

FileID FileContentSnapshot::path_to_id(const std::string& path) const {
    auto it = path_index.find(path);
    if (it == path_index.end()) return 0;
    return entries[it->second].file_id;
}

void FileContentSnapshot::rebuild_indices() {
    id_index.clear();
    path_index.clear();
    id_index.reserve(entries.size());
    path_index.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        id_index[e.file_id] = i;
        path_index[e.path] = i;
    }
}

// -- compute_line_offsets -----------------------------------------------------

std::vector<uint32_t> compute_line_offsets(std::string_view content) {
    if (content.empty()) return {};

    size_t estimated_lines = content.size() / 80 + 2;
    if (estimated_lines > 1000) estimated_lines = 1000;

    std::vector<uint32_t> offsets;
    offsets.reserve(estimated_lines);
    offsets.push_back(0);

    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n' && i + 1 < content.size()) {
            offsets.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    return offsets;
}

// -- FileContentStore ---------------------------------------------------------

FileContentStore::FileContentStore(int64_t max_memory_bytes)
    : max_memory_bytes_(max_memory_bytes) {
    snapshot_.store(std::make_shared<FileContentSnapshot>());
}

std::shared_ptr<FileContentSnapshot> FileContentStore::load_snapshot() const {
    return snapshot_.load(std::memory_order_acquire);
}

std::shared_ptr<FileContent> FileContentStore::make_file_content(FileID id,
                                                                  std::string_view data) const {
    auto fc = std::make_shared<FileContent>();
    fc->file_id = id;
    fc->content.assign(data.begin(), data.end());
    fc->line_offsets = compute_line_offsets(data);
    fc->fast_hash = XXH64(data.data(), data.size(), 0);
    // content_hash (SHA-256) is computed lazily by get_content_hash —
    // production code never reads it (only tests do, and we want SHA on
    // demand). perf record showed compute_sha256 dominating in malloc
    // (per-file pad buffer alloc) and CPU (block-iter); 2772 files at
    // ~100 blocks each compounded to a measurable fraction of indexing
    // wall time. Lazy compute keeps the field semantically populated
    // without the eager cost. content_hash is left zero-initialised
    // here; get_content_hash fills it on first read.
    fc->ref_count.store(1, std::memory_order_relaxed);
    return fc;
}

int64_t FileContentStore::estimate_memory(const FileContent& fc) {
    return static_cast<int64_t>(fc.content.size()) +
           static_cast<int64_t>(fc.line_offsets.size()) * 4 + 64;
}

void FileContentStore::enforce_memory_limit(std::shared_ptr<FileContentSnapshot>& snap) {
    if (max_memory_bytes_ <= 0) return;

    int64_t current = current_memory_.load(std::memory_order_relaxed);
    if (current <= max_memory_bytes_) return;

    // Collect ids to evict in a first pass using the read-only id_index,
    // then erase them in a single pass. Erasing one at a time would
    // invalidate id_index positions for subsequent lookups.
    absl::flat_hash_set<FileID> evict_set;
    size_t evict_idx = 0;
    while (current > max_memory_bytes_ && evict_idx < snap->access_order.size()) {
        FileID evict_id = snap->access_order[evict_idx];
        ++evict_idx;

        auto map_it = snap->id_index.find(evict_id);
        if (map_it == snap->id_index.end()) continue;

        size_t pos = map_it->second;
        int64_t file_size = estimate_memory(*snap->entries[pos].content);
        current -= file_size;
        current_memory_.fetch_sub(file_size, std::memory_order_relaxed);
        evict_set.insert(evict_id);
    }

    if (evict_set.empty() && evict_idx == 0) return;

    if (!evict_set.empty()) {
        snap->entries.erase(
            std::remove_if(snap->entries.begin(), snap->entries.end(),
                           [&](const FileContentSnapshot::Entry& e) {
                               return evict_set.contains(e.file_id);
                           }),
            snap->entries.end());
    }

    if (evict_idx > 0) {
        snap->access_order.erase(snap->access_order.begin(),
                                 snap->access_order.begin() +
                                     static_cast<ptrdiff_t>(evict_idx));
    }

    // Erasing entries shifts positions, so the index maps must be rebuilt.
    if (!evict_set.empty()) {
        snap->rebuild_indices();
    }
}

// -- Lock-free read API -------------------------------------------------------

std::shared_ptr<FileContent> FileContentStore::get_file(FileID id) const {
    auto snap = load_snapshot();
    return snap->find_by_id(id);
}

std::string_view FileContentStore::get_content(FileID id) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(id);
    if (!fc) return {};
    return fc->view();
}

const std::vector<uint32_t>* FileContentStore::get_line_offsets(FileID id) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(id);
    if (!fc) return nullptr;
    return &fc->line_offsets;
}

std::string_view FileContentStore::get_string(StringRef ref) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(ref.file_id);
    if (!fc) return {};
    auto view = fc->view();
    uint32_t end = ref.offset + ref.length;
    if (ref.offset >= view.size() || end > view.size()) return {};
    return view.substr(ref.offset, ref.length);
}

StringRef FileContentStore::get_line(FileID file_id, int line_num) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(file_id);
    if (!fc) return {};

    const auto& offsets = fc->line_offsets;
    if (line_num < 0 || line_num >= static_cast<int>(offsets.size())) return {};

    uint32_t start = offsets[static_cast<size_t>(line_num)];
    uint32_t end;

    if (line_num + 1 < static_cast<int>(offsets.size())) {
        end = offsets[static_cast<size_t>(line_num + 1)];
        if (end > start && fc->content[end - 1] == '\n') {
            --end;
        }
    } else {
        end = static_cast<uint32_t>(fc->content.size());
    }

    uint32_t length = end - start;
    uint64_t h = 0;
    if (length > 0) {
        auto view = fc->view();
        h = hash_fnv1a(view.substr(start, length));
    }

    return {file_id, start, length, h};
}

int FileContentStore::get_line_count(FileID file_id) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(file_id);
    if (!fc) return 0;
    return static_cast<int>(fc->line_offsets.size());
}

std::array<uint8_t, 32> FileContentStore::get_content_hash(FileID file_id) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(file_id);
    if (!fc) return {};
    // Lazy SHA-256: compute on first read, cache on the FileContent.
    // Detected via the zero-initialised array sentinel — collision with
    // a real SHA-256 of all-zero output is theoretically possible but
    // requires data that hashes to that specific 32-byte zero value;
    // for indexing purposes treat zero as "not computed".
    static constexpr std::array<uint8_t, 32> kZeroHash{};
    if (fc->content_hash == kZeroHash) {
        // Cache under the same FileContent — mutable from the
        // get_content_hash caller's POV. Concurrent first-readers may
        // each compute and overwrite, but the result is identical
        // (deterministic hash of the same content) so the race is
        // benign.
        auto* mutable_fc = const_cast<FileContent*>(fc.get());
        mutable_fc->content_hash = compute_sha256(
            std::string_view(reinterpret_cast<const char*>(fc->content.data()),
                             fc->content.size()));
    }
    return fc->content_hash;
}

uint64_t FileContentStore::get_fast_hash(FileID file_id) const {
    auto snap = load_snapshot();
    const auto& fc = snap->find_by_id(file_id);
    if (!fc) return 0;
    return fc->fast_hash;
}

int FileContentStore::get_file_count() const {
    auto snap = load_snapshot();
    return static_cast<int>(snap->entries.size());
}

int64_t FileContentStore::get_memory_usage() const {
    return current_memory_.load(std::memory_order_relaxed);
}

FileID FileContentStore::path_to_id(const std::string& path) const {
    auto snap = load_snapshot();
    return snap->path_to_id(path);
}

// -- Write API ----------------------------------------------------------------

FileID FileContentStore::load_file(const std::string& path, std::string_view content) {
    // Serialize the load-mutate-swap so concurrent writers cannot drop
    // entries by basing their copy on an out-of-date snapshot.
    std::lock_guard<std::mutex> write_lock(write_mu_);

    auto snap = std::make_shared<FileContentSnapshot>(*load_snapshot());

    uint64_t new_hash = XXH64(content.data(), content.size(), 0);

    // Check if file exists and content unchanged.
    FileID existing_id = snap->path_to_id(path);
    if (existing_id != 0) {
        const auto& existing_fc = snap->find_by_id(existing_id);
        if (existing_fc && existing_fc->fast_hash == new_hash) {
            return existing_id;
        }
    }

    FileID file_id;
    if (existing_id != 0) {
        file_id = existing_id;
        // O(1) lookup of old entry via id_index; replace content in place.
        // Position is preserved so id_index / path_index stay valid.
        auto map_it = snap->id_index.find(file_id);
        if (map_it != snap->id_index.end()) {
            auto& entry = snap->entries[map_it->second];
            int64_t old_size = estimate_memory(*entry.content);
            current_memory_.fetch_sub(old_size, std::memory_order_relaxed);
            auto fc = make_file_content(file_id, content);
            int64_t new_size = estimate_memory(*fc);
            current_memory_.fetch_add(new_size, std::memory_order_relaxed);
            entry.content = std::move(fc);
        }
    } else {
        file_id = static_cast<FileID>(next_id_.fetch_add(1, std::memory_order_relaxed) + 1);
        auto fc = make_file_content(file_id, content);
        int64_t new_size = estimate_memory(*fc);
        current_memory_.fetch_add(new_size, std::memory_order_relaxed);
        size_t new_pos = snap->entries.size();
        snap->entries.push_back({file_id, path, std::move(fc)});
        snap->id_index[file_id] = new_pos;
        snap->path_index[path] = new_pos;
    }

    snap->access_order.push_back(file_id);
    enforce_memory_limit(snap);
    snapshot_.store(std::move(snap), std::memory_order_release);
    return file_id;
}

std::vector<FileID> FileContentStore::batch_load_files(
    const std::vector<std::pair<std::string, std::string_view>>& files) {
    if (files.empty()) return {};

    std::lock_guard<std::mutex> write_lock(write_mu_);

    auto snap = std::make_shared<FileContentSnapshot>(*load_snapshot());
    std::vector<FileID> ids;
    ids.reserve(files.size());
    int64_t total_delta = 0;

    for (const auto& [path, content] : files) {
        uint64_t new_hash = XXH64(content.data(), content.size(), 0);

        FileID existing_id = snap->path_to_id(path);
        if (existing_id != 0) {
            const auto& existing_fc = snap->find_by_id(existing_id);
            if (existing_fc && existing_fc->fast_hash == new_hash) {
                ids.push_back(existing_id);
                continue;
            }
        }

        FileID file_id;
        if (existing_id != 0) {
            file_id = existing_id;
            // O(1) lookup; replace in place to keep index positions stable.
            auto map_it = snap->id_index.find(file_id);
            if (map_it != snap->id_index.end()) {
                auto& entry = snap->entries[map_it->second];
                total_delta -= estimate_memory(*entry.content);
                auto fc = make_file_content(file_id, content);
                total_delta += estimate_memory(*fc);
                entry.content = std::move(fc);
            }
        } else {
            file_id = static_cast<FileID>(next_id_.fetch_add(1, std::memory_order_relaxed) + 1);
            auto fc = make_file_content(file_id, content);
            total_delta += estimate_memory(*fc);
            size_t new_pos = snap->entries.size();
            snap->entries.push_back({file_id, path, std::move(fc)});
            snap->id_index[file_id] = new_pos;
            snap->path_index[path] = new_pos;
        }

        snap->access_order.push_back(file_id);
        ids.push_back(file_id);
    }

    if (total_delta != 0) {
        current_memory_.fetch_add(total_delta, std::memory_order_relaxed);
    }

    enforce_memory_limit(snap);
    snapshot_.store(std::move(snap), std::memory_order_release);
    return ids;
}

void FileContentStore::invalidate_file(const std::string& path) {
    std::lock_guard<std::mutex> write_lock(write_mu_);

    auto snap = std::make_shared<FileContentSnapshot>(*load_snapshot());
    FileID id = snap->path_to_id(path);
    if (id == 0) return;

    const auto& fc = snap->find_by_id(id);
    if (fc) {
        current_memory_.fetch_sub(estimate_memory(*fc), std::memory_order_relaxed);
    }

    snap->entries.erase(
        std::remove_if(snap->entries.begin(), snap->entries.end(),
                       [id](const FileContentSnapshot::Entry& e) { return e.file_id == id; }),
        snap->entries.end());

    snap->access_order.erase(
        std::remove(snap->access_order.begin(), snap->access_order.end(), id),
        snap->access_order.end());

    // Erase shifts subsequent positions, so the index maps must be rebuilt.
    snap->rebuild_indices();
    snapshot_.store(std::move(snap), std::memory_order_release);
}

void FileContentStore::invalidate_file_by_id(FileID file_id) {
    std::lock_guard<std::mutex> write_lock(write_mu_);

    auto snap = std::make_shared<FileContentSnapshot>(*load_snapshot());
    const auto& fc = snap->find_by_id(file_id);
    if (!fc) return;

    current_memory_.fetch_sub(estimate_memory(*fc), std::memory_order_relaxed);

    snap->entries.erase(
        std::remove_if(
            snap->entries.begin(), snap->entries.end(),
            [file_id](const FileContentSnapshot::Entry& e) { return e.file_id == file_id; }),
        snap->entries.end());

    snap->access_order.erase(
        std::remove(snap->access_order.begin(), snap->access_order.end(), file_id),
        snap->access_order.end());

    // Erase shifts subsequent positions, so the index maps must be rebuilt.
    snap->rebuild_indices();
    snapshot_.store(std::move(snap), std::memory_order_release);
}

void FileContentStore::clear() {
    std::lock_guard<std::mutex> write_lock(write_mu_);
    snapshot_.store(std::make_shared<FileContentSnapshot>(), std::memory_order_release);
    current_memory_.store(0, std::memory_order_relaxed);
    next_id_.store(0, std::memory_order_relaxed);
}

}  // namespace lci
