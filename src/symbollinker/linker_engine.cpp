#include <lci/symbollinker/linker_engine.h>

#include <algorithm>
#include <chrono>

#include <tree_sitter/api.h>
#include <xxhash.h>

#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>

namespace lci::symbollinker {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LinkerEngine::LinkerEngine(std::string root_path)
    : root_path_(std::move(root_path)) {}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void LinkerEngine::register_extractor(
    std::unique_ptr<SymbolExtractor> extractor) {
    auto lang = extractor->language();
    extractors_[static_cast<int>(lang)] = std::move(extractor);
}

void LinkerEngine::register_resolver(
    std::unique_ptr<ImportResolver> resolver) {
    auto lang = resolver->language();
    resolvers_[static_cast<int>(lang)] = std::move(resolver);
}

// ---------------------------------------------------------------------------
// File management
// ---------------------------------------------------------------------------

FileID LinkerEngine::get_or_create_file_id(std::string_view path) {
    std::string key(path);
    auto it = file_registry_.find(key);
    if (it != file_registry_.end()) {
        return it->second;
    }

    FileID id = next_file_id_++;
    file_registry_[key] = id;
    reverse_registry_[id] = key;
    return id;
}

std::string_view LinkerEngine::get_file_path(FileID file_id) const {
    auto it = reverse_registry_.find(file_id);
    if (it != reverse_registry_.end()) {
        return it->second;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Indexing
// ---------------------------------------------------------------------------

bool LinkerEngine::index_file(std::string_view path,
                              std::string_view content) {
    SymbolExtractor* extractor = find_extractor(path);
    if (extractor == nullptr) {
        return false;
    }

    FileID file_id = get_or_create_file_id(path);

    parser::PooledParser pooled(extractor->language());
    if (!pooled) {
        return false;
    }

    TSTree* tree = ts_parser_parse_string(
        pooled.get(), nullptr, content.data(),
        static_cast<uint32_t>(content.size()));
    if (tree == nullptr) {
        return false;
    }

    SymbolTable table = extractor->extract_symbols(file_id, content, tree);
    ts_tree_delete(tree);

    symbol_tables_[file_id] = std::move(table);
    return true;
}

// ---------------------------------------------------------------------------
// Linking
// ---------------------------------------------------------------------------

bool LinkerEngine::link_symbols() {
    symbol_links_.clear();
    import_links_.clear();

    for (auto& [file_id, table] : symbol_tables_) {
        if (!process_file_links(file_id, table)) {
            return false;
        }
    }

    for (auto& [file_id, _] : symbol_tables_) {
        update_dependency_graph(file_id);
    }

    return true;
}

bool LinkerEngine::process_file_links(FileID file_id,
                                      const SymbolTable& table) {
    for (const auto& import_info : table.imports) {
        process_import(file_id, import_info, table.language);
    }

    for (size_t i = 0; i < table.symbol_ids.size(); ++i) {
        SymbolID sym_id = table.symbol_ids[i];
        if (symbol_links_.find(sym_id) == symbol_links_.end()) {
            SymbolLink link;
            link.symbol = sym_id;
            link.definition_file = file_id;
            symbol_links_[sym_id] = std::move(link);
        }

        const std::string& sym_name = table.symbol_names[i];
        for (const auto& export_info : table.exports) {
            if (export_info.local_name == sym_name ||
                export_info.exported_name == sym_name) {
                symbol_links_[sym_id].exported_by = file_id;
                break;
            }
        }
    }

    return true;
}

void LinkerEngine::process_import(FileID file_id,
                                  const ImportInfo& import_info,
                                  parser::Language language) {
    ImportResolver* resolver = find_resolver(language);

    ModuleResolution resolution;
    if (resolver != nullptr) {
        resolution =
            resolver->resolve_import(import_info.import_path, file_id);
    } else {
        resolution.is_external = true;
        resolution.request_path = import_info.import_path;
    }

    ImportLink link;
    link.from_file = file_id;
    link.import_path = import_info.import_path;
    link.resolved_file = resolution.file_id;
    link.imported_symbols = import_info.imported_names;
    link.resolution = resolution;
    link.is_external = resolution.is_external;

    import_links_[file_id].push_back(std::move(link));

    if (!resolution.is_external && resolution.file_id != 0) {
        auto target_it = symbol_tables_.find(resolution.file_id);
        if (target_it != symbol_tables_.end()) {
            const SymbolTable& target = target_it->second;
            for (const auto& name : import_info.imported_names) {
                for (size_t j = 0; j < target.symbol_names.size(); ++j) {
                    if (target.symbol_names[j] == name) {
                        SymbolID target_sym = target.symbol_ids[j];
                        auto& sym_link = symbol_links_[target_sym];
                        if (!contains_file_id(sym_link.imported_by,
                                              file_id)) {
                            sym_link.imported_by.push_back(file_id);
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Incremental updates
// ---------------------------------------------------------------------------

UpdateResult LinkerEngine::update_file(std::string_view path,
                                       std::string_view content) {
    auto start = std::chrono::steady_clock::now();

    FileID file_id = get_or_create_file_id(path);
    uint64_t new_hash =
        XXH64(content.data(), content.size(), 0);

    auto hash_it = file_hashes_.find(file_id);
    if (hash_it != file_hashes_.end() && hash_it->second == new_hash) {
        UpdateResult result;
        result.duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }

    UpdateType utype = (hash_it == file_hashes_.end()) ? UpdateType::Added
                                                       : UpdateType::Modified;

    UpdateInfo info;
    info.file_id = file_id;
    info.content = std::string(content);
    info.content_hash = new_hash;
    info.update_type = utype;

    pending_updates_[file_id] = std::move(info);

    UpdateResult result = process_pending_updates();
    result.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

UpdateResult LinkerEngine::remove_file(std::string_view path) {
    auto start = std::chrono::steady_clock::now();

    std::string key(path);
    auto it = file_registry_.find(key);
    if (it == file_registry_.end()) {
        UpdateResult result;
        result.duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }

    FileID file_id = it->second;

    UpdateInfo info;
    info.file_id = file_id;
    info.update_type = UpdateType::Removed;

    pending_updates_[file_id] = std::move(info);

    UpdateResult result = process_pending_updates();
    result.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
}

UpdateResult LinkerEngine::process_pending_updates() {
    UpdateResult result;
    int cascade_depth = 0;
    constexpr int kMaxCascadeDepth = 10;

    while (!pending_updates_.empty()) {
        std::vector<FileID> batch;
        batch.reserve(pending_updates_.size());
        for (auto& [fid, _] : pending_updates_) {
            batch.push_back(fid);
        }

        absl::flat_hash_map<FileID, UpdateInfo> cascade_updates;

        for (FileID fid : batch) {
            auto upd_it = pending_updates_.find(fid);
            if (upd_it == pending_updates_.end()) {
                continue;
            }
            UpdateInfo info = std::move(upd_it->second);
            pending_updates_.erase(upd_it);

            switch (info.update_type) {
                case UpdateType::Added:
                case UpdateType::Modified:
                    process_file_add_or_modify(info, result);
                    break;
                case UpdateType::Removed:
                    process_file_removal(info, result);
                    break;
                case UpdateType::Cascade:
                    process_file_cascade(info, result);
                    break;
            }

            find_cascade_updates(fid, cascade_updates);
        }

        for (auto& [fid, info] : cascade_updates) {
            pending_updates_[fid] = std::move(info);
        }

        ++cascade_depth;
        if (cascade_depth > kMaxCascadeDepth) {
            break;
        }
    }

    result.cascade_depth = cascade_depth;
    return result;
}

bool LinkerEngine::process_file_add_or_modify(const UpdateInfo& info,
                                              UpdateResult& result) {
    FileID file_id = info.file_id;
    std::string_view path = get_file_path(file_id);

    int old_symbol_count = 0;
    auto old_it = symbol_tables_.find(file_id);
    if (old_it != symbol_tables_.end()) {
        old_symbol_count =
            static_cast<int>(old_it->second.symbol_ids.size());
    }

    if (!index_file(path, info.content)) {
        return false;
    }

    file_hashes_[file_id] = info.content_hash;

    auto new_it = symbol_tables_.find(file_id);
    int new_symbol_count = 0;
    if (new_it != symbol_tables_.end()) {
        new_symbol_count =
            static_cast<int>(new_it->second.symbol_ids.size());
    }

    if (new_symbol_count > old_symbol_count) {
        result.added_symbols += new_symbol_count - old_symbol_count;
    } else {
        result.removed_symbols += old_symbol_count - new_symbol_count;
    }

    update_dependency_graph(file_id);
    result.updated_files.push_back(file_id);
    return true;
}

void LinkerEngine::process_file_removal(const UpdateInfo& info,
                                        UpdateResult& result) {
    FileID file_id = info.file_id;

    auto table_it = symbol_tables_.find(file_id);
    if (table_it != symbol_tables_.end()) {
        result.removed_symbols +=
            static_cast<int>(table_it->second.symbol_ids.size());
        for (SymbolID sym_id : table_it->second.symbol_ids) {
            symbol_links_.erase(sym_id);
        }
        symbol_tables_.erase(table_it);
    }

    file_hashes_.erase(file_id);
    import_links_.erase(file_id);
    import_graph_.erase(file_id);

    for (auto& [other_id, deps] : file_dependents_) {
        remove_file_from_vec(deps, file_id);
    }
    file_dependents_.erase(file_id);

    for (auto& [other_id, imports] : import_graph_) {
        remove_file_from_vec(imports, file_id);
    }

    result.updated_files.push_back(file_id);
}

void LinkerEngine::process_file_cascade(const UpdateInfo& info,
                                        UpdateResult& result) {
    FileID file_id = info.file_id;

    auto table_it = symbol_tables_.find(file_id);
    if (table_it != symbol_tables_.end()) {
        import_links_.erase(file_id);
        process_file_links(file_id, table_it->second);
    }

    update_dependency_graph(file_id);
    result.affected_files.push_back(file_id);
    result.modified_links++;
}

void LinkerEngine::find_cascade_updates(
    FileID changed_file,
    absl::flat_hash_map<FileID, UpdateInfo>& cascade_updates) const {
    auto dep_it = file_dependents_.find(changed_file);
    if (dep_it == file_dependents_.end()) {
        return;
    }

    for (FileID dependent : dep_it->second) {
        if (pending_updates_.find(dependent) != pending_updates_.end()) {
            continue;
        }
        if (cascade_updates.find(dependent) != cascade_updates.end()) {
            continue;
        }

        UpdateInfo info;
        info.file_id = dependent;
        info.update_type = UpdateType::Cascade;
        cascade_updates[dependent] = std::move(info);
    }
}

// ---------------------------------------------------------------------------
// Dependency graph
// ---------------------------------------------------------------------------

void LinkerEngine::update_dependency_graph(FileID file_id) {
    auto old_it = import_graph_.find(file_id);
    if (old_it != import_graph_.end()) {
        for (FileID imported : old_it->second) {
            auto dep_it = file_dependents_.find(imported);
            if (dep_it != file_dependents_.end()) {
                remove_file_from_vec(dep_it->second, file_id);
            }
        }
    }

    std::vector<FileID> new_imports;
    auto links_it = import_links_.find(file_id);
    if (links_it != import_links_.end()) {
        for (const ImportLink& link : links_it->second) {
            if (link.resolved_file != 0 && !link.is_external) {
                new_imports.push_back(link.resolved_file);

                auto& deps = file_dependents_[link.resolved_file];
                if (!contains_file_id(deps, file_id)) {
                    deps.push_back(file_id);
                }
            }
        }
    }

    import_graph_[file_id] = std::move(new_imports);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<const ImportLink*>
LinkerEngine::get_file_imports(FileID file_id) const {
    std::vector<const ImportLink*> result;
    auto it = import_links_.find(file_id);
    if (it != import_links_.end()) {
        result.reserve(it->second.size());
        for (const ImportLink& link : it->second) {
            result.push_back(&link);
        }
    }
    return result;
}

std::vector<FileID>
LinkerEngine::get_file_dependents(FileID file_id) const {
    auto it = file_dependents_.find(file_id);
    if (it != file_dependents_.end()) {
        return it->second;
    }
    return {};
}

std::vector<FileID>
LinkerEngine::get_file_dependencies(FileID file_id) const {
    auto it = import_graph_.find(file_id);
    if (it != import_graph_.end()) {
        return it->second;
    }
    return {};
}

uint64_t LinkerEngine::get_file_hash(FileID file_id) const {
    auto it = file_hashes_.find(file_id);
    if (it != file_hashes_.end()) {
        return it->second;
    }
    return 0;
}

const SymbolTable* LinkerEngine::get_symbol_table(FileID file_id) const {
    auto it = symbol_tables_.find(file_id);
    if (it != symbol_tables_.end()) {
        return &it->second;
    }
    return nullptr;
}

LinkerStats LinkerEngine::stats() const {
    LinkerStats s;
    s.files = static_cast<int>(symbol_tables_.size());
    s.symbol_links = static_cast<int>(symbol_links_.size());
    s.extractors = 0;
    for (const auto& e : extractors_) {
        if (e) {
            ++s.extractors;
        }
    }
    s.resolvers = 0;
    for (const auto& r : resolvers_) {
        if (r) {
            ++s.resolvers;
        }
    }
    s.import_links = 0;
    for (const auto& [_, links] : import_links_) {
        s.import_links += static_cast<int>(links.size());
    }
    s.dependency_edges = 0;
    for (const auto& [_, deps] : file_dependents_) {
        s.dependency_edges += static_cast<int>(deps.size());
    }
    return s;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

SymbolExtractor* LinkerEngine::find_extractor(std::string_view path) const {
    for (const auto& e : extractors_) {
        if (e && e->can_handle(path)) {
            return e.get();
        }
    }
    return nullptr;
}

ImportResolver* LinkerEngine::find_resolver(parser::Language lang) const {
    int idx = static_cast<int>(lang);
    if (idx >= 0 && idx < parser::kLanguageCount && resolvers_[idx]) {
        return resolvers_[idx].get();
    }
    return nullptr;
}

void LinkerEngine::remove_file_from_vec(std::vector<FileID>& vec,
                                        FileID id) {
    vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
}

bool LinkerEngine::contains_file_id(const std::vector<FileID>& vec,
                                    FileID id) {
    return std::find(vec.begin(), vec.end(), id) != vec.end();
}

}  // namespace lci::symbollinker
