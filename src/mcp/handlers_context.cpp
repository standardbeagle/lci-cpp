#include <lci/mcp/handlers_context.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include <lci/indexing/master_index.h>
#include <lci/mcp/context_manifest_expander.h>

namespace lci {
namespace mcp {

// -- JSON serialization -------------------------------------------------------

nlohmann::json manifest_to_json(const ContextManifest& m) {
    nlohmann::json j;
    if (!m.task.empty()) j["task"] = m.task;
    if (!m.version.empty()) j["version"] = m.version;
    if (!m.project_root.empty()) j["project_root"] = m.project_root;

    auto& refs = j["refs"];
    refs = nlohmann::json::array();
    for (const auto& r : m.refs) {
        nlohmann::json rj;
        rj["f"] = r.file;
        if (!r.symbol.empty()) rj["s"] = r.symbol;
        if (r.has_line_range) {
            rj["l"] = {{"start", r.line_range.start},
                       {"end", r.line_range.end}};
        }
        if (!r.role.empty()) rj["role"] = r.role;
        if (!r.note.empty()) rj["note"] = r.note;
        if (!r.expansions.empty()) rj["x"] = r.expansions;
        refs.push_back(std::move(rj));
    }

    return j;
}

std::string manifest_from_json(const nlohmann::json& j,
                               ContextManifest& out) {
    out = {};
    if (j.contains("task") && j["task"].is_string()) {
        out.task = j["task"].get<std::string>();
    }
    if (j.contains("version") && j["version"].is_string()) {
        out.version = j["version"].get<std::string>();
    }
    if (j.contains("project_root") && j["project_root"].is_string()) {
        out.project_root = j["project_root"].get<std::string>();
    }

    if (!j.contains("refs") || !j["refs"].is_array()) {
        return "missing or invalid 'refs' array";
    }

    for (const auto& rj : j["refs"]) {
        ContextRef r;
        if (rj.contains("f") && rj["f"].is_string()) {
            r.file = rj["f"].get<std::string>();
        }
        if (rj.contains("s") && rj["s"].is_string()) {
            r.symbol = rj["s"].get<std::string>();
        }
        if (rj.contains("l") && rj["l"].is_object()) {
            r.line_range.start = rj["l"].value("start", 0);
            r.line_range.end = rj["l"].value("end", 0);
            r.has_line_range = true;
        }
        if (rj.contains("role") && rj["role"].is_string()) {
            r.role = rj["role"].get<std::string>();
        }
        if (rj.contains("note") && rj["note"].is_string()) {
            r.note = rj["note"].get<std::string>();
        }
        if (rj.contains("x") && rj["x"].is_array()) {
            for (const auto& x : rj["x"]) {
                if (x.is_string()) {
                    r.expansions.push_back(x.get<std::string>());
                }
            }
        }
        out.refs.push_back(std::move(r));
    }

    return {};
}

std::string validate_manifest(const ContextManifest& m) {
    if (m.refs.empty()) {
        return "manifest must have at least one reference";
    }
    for (size_t i = 0; i < m.refs.size(); ++i) {
        const auto& r = m.refs[i];
        if (r.file.empty() && r.symbol.empty() && !r.has_line_range) {
            return "ref[" + std::to_string(i) +
                   "] must have file, symbol, or line range";
        }
    }
    return {};
}

ManifestStats compute_manifest_stats(const ContextManifest& m) {
    ManifestStats stats;
    stats.ref_count = static_cast<int>(m.refs.size());

    absl::flat_hash_set<std::string> files;
    int total_lines = 0;
    for (const auto& r : m.refs) {
        if (!r.file.empty()) files.insert(r.file);
        if (r.has_line_range) {
            total_lines += r.line_range.end - r.line_range.start + 1;
        }
    }
    stats.file_count = static_cast<int>(files.size());
    stats.total_lines = total_lines;

    return stats;
}

nlohmann::json hydrated_context_to_json(const HydratedContext& ctx) {
    nlohmann::json j;
    if (!ctx.task.empty()) j["task"] = ctx.task;

    auto& refs = j["refs"];
    refs = nlohmann::json::array();
    for (const auto& r : ctx.refs) {
        nlohmann::json rj;
        rj["file"] = r.file;
        if (!r.symbol.empty()) rj["symbol"] = r.symbol;
        rj["lines"] = {{"start", r.lines.start}, {"end", r.lines.end}};
        if (!r.role.empty()) rj["role"] = r.role;
        if (!r.note.empty()) rj["note"] = r.note;
        rj["source"] = r.source;
        if (!r.symbol_type.empty()) rj["symbol_type"] = r.symbol_type;
        if (!r.signature.empty()) rj["signature"] = r.signature;
        if (r.is_exported) rj["is_exported"] = true;
        if (r.is_external) rj["is_external"] = true;
        refs.push_back(std::move(rj));
    }

    j["stats"] = {{"refs_loaded", ctx.stats.refs_loaded},
                  {"symbols_hydrated", ctx.stats.symbols_hydrated},
                  {"tokens_approx", ctx.stats.tokens_approx},
                  {"expansions_applied", ctx.stats.expansions_applied},
                  {"truncated", ctx.stats.truncated}};

    if (!ctx.warnings.empty()) j["warnings"] = ctx.warnings;

    return j;
}

// -- Internal helpers ---------------------------------------------------------

namespace {

/// Resolves a manifest path relative to the project root.
std::string resolve_manifest_path(const std::string& relative_path,
                                   const std::string& project_root) {
    if (relative_path.empty()) return {};
    if (!relative_path.empty() && relative_path[0] == '/') {
        return relative_path;
    }
    auto root = project_root.empty() ? "." : project_root;
    return root + "/" + relative_path;
}

/// Saves a manifest to a file atomically.
std::string save_manifest_to_file(const ContextManifest& manifest,
                                   const std::string& file_path) {
    namespace fs = std::filesystem;

    auto dir = fs::path(file_path).parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return "failed to create directory: " + ec.message();
    }

    auto data = manifest_to_json(manifest);
    auto json_str = data.dump(2);

    auto temp_path = file_path + ".tmp";
    {
        std::ofstream out(temp_path, std::ios::binary);
        if (!out) {
            return "failed to write temp file: " + temp_path;
        }
        out.write(json_str.data(),
                  static_cast<std::streamsize>(json_str.size()));
        if (!out) {
            return "failed to write manifest data";
        }
    }

    fs::rename(temp_path, file_path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return "failed to rename temp file: " + ec.message();
    }

    return {};
}

/// Loads a manifest from a file.
std::string load_manifest_from_file(const std::string& file_path,
                                     ContextManifest& out) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        return "file not found: " + file_path;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(content);
    } catch (const nlohmann::json::parse_error& e) {
        return std::string("invalid manifest JSON: ") + e.what();
    }

    auto err = manifest_from_json(j, out);
    if (!err.empty()) return err;

    return validate_manifest(out);
}

/// Filters refs by role inclusion/exclusion lists.
std::vector<ContextRef> filter_refs_by_role(
    const std::vector<ContextRef>& refs,
    const std::vector<std::string>& include,
    const std::vector<std::string>& exclude) {
    if (include.empty() && exclude.empty()) return refs;

    absl::flat_hash_set<std::string> include_set(include.begin(),
                                                  include.end());
    absl::flat_hash_set<std::string> exclude_set(exclude.begin(),
                                                  exclude.end());

    std::vector<ContextRef> filtered;
    filtered.reserve(refs.size());

    for (const auto& ref : refs) {
        if (!exclude.empty() && exclude_set.contains(ref.role)) continue;
        if (!include.empty() && !include_set.contains(ref.role)) continue;
        filtered.push_back(ref);
    }

    return filtered;
}

/// Parses a format string to FormatType.
FormatType parse_format(const std::string& fmt) {
    if (fmt == "signatures") return FormatType::Signatures;
    if (fmt == "outline") return FormatType::Outline;
    return FormatType::Full;
}

// -- Save handler -------------------------------------------------------------

ToolResult handle_context_save(const nlohmann::json& params,
                               const std::string& project_root) {
    // Extract refs
    if (!params.contains("refs") || !params["refs"].is_array() ||
        params["refs"].empty()) {
        return make_error_response(
            "context",
            "must provide 'refs' with at least one reference");
    }

    auto to_file = params.value("to_file", "");
    auto to_string_flag = params.value("to_string", false);

    if (to_file.empty() && !to_string_flag) {
        return make_error_response(
            "context",
            "must provide either 'to_file' or set 'to_string' to true");
    }

    // Build manifest
    ContextManifest manifest;
    manifest.task = params.value("task", "");
    manifest.project_root = project_root;

    auto err = manifest_from_json(params, manifest);
    // manifest_from_json may partially fill; re-validate
    if (manifest.refs.empty()) {
        // Try direct parse of refs array
        manifest.refs.clear();
        for (const auto& rj : params["refs"]) {
            ContextRef r;
            r.file = rj.value("f", "");
            r.symbol = rj.value("s", "");
            if (rj.contains("l") && rj["l"].is_object()) {
                r.line_range.start = rj["l"].value("start", 0);
                r.line_range.end = rj["l"].value("end", 0);
                r.has_line_range = true;
            }
            r.role = rj.value("role", "");
            r.note = rj.value("note", "");
            if (rj.contains("x") && rj["x"].is_array()) {
                for (const auto& x : rj["x"]) {
                    if (x.is_string()) {
                        r.expansions.push_back(x.get<std::string>());
                    }
                }
            }
            manifest.refs.push_back(std::move(r));
        }
    }

    auto val_err = validate_manifest(manifest);
    if (!val_err.empty()) {
        return make_error_response("context", "invalid manifest: " + val_err);
    }

    // Handle append mode
    bool append = params.value("append", false);
    if (append && !to_file.empty()) {
        auto full_path = resolve_manifest_path(to_file, project_root);
        ContextManifest existing;
        auto load_err = load_manifest_from_file(full_path, existing);
        if (load_err.empty()) {
            // Merge: prepend existing refs
            existing.refs.insert(existing.refs.end(),
                                 manifest.refs.begin(),
                                 manifest.refs.end());
            if (manifest.task.empty()) {
                manifest.task = existing.task;
            }
            manifest.refs = std::move(existing.refs);
        }
        // If file doesn't exist, that's fine for append
    }

    auto stats = compute_manifest_stats(manifest);

    if (!to_file.empty()) {
        auto full_path = resolve_manifest_path(to_file, project_root);
        auto save_err = save_manifest_to_file(manifest, full_path);
        if (!save_err.empty()) {
            return make_error_response("context",
                                       "failed to save manifest: " + save_err);
        }

        nlohmann::json response;
        response["saved"] = to_file;
        response["stats"] = {{"ref_count", stats.ref_count},
                             {"file_count", stats.file_count},
                             {"total_lines", stats.total_lines}};
        response["ref_count"] = stats.ref_count;
        response["file_count"] = stats.file_count;
        return make_json_response(response);
    }

    // Return as string
    auto manifest_json = manifest_to_json(manifest);
    nlohmann::json response;
    response["manifest"] = manifest_json.dump(2);
    response["stats"] = {{"ref_count", stats.ref_count},
                         {"file_count", stats.file_count},
                         {"total_lines", stats.total_lines}};
    response["ref_count"] = stats.ref_count;
    response["file_count"] = stats.file_count;
    return make_json_response(response);
}

// -- Load handler -------------------------------------------------------------

ToolResult handle_context_load(const nlohmann::json& params,
                               MasterIndex& indexer,
                               const std::string& project_root) {
    auto from_file = params.value("from_file", "");
    auto from_string = params.value("from_string", "");

    if (from_file.empty() && from_string.empty()) {
        return make_error_response(
            "context",
            "must provide either 'from_file' or 'from_string'");
    }

    auto format_str = params.value("format", "full");
    auto format = parse_format(format_str);

    // Load manifest
    ContextManifest manifest;
    if (!from_file.empty()) {
        auto full_path = resolve_manifest_path(from_file, project_root);
        auto err = load_manifest_from_file(full_path, manifest);
        if (!err.empty()) {
            return make_error_response(
                "context", "failed to load manifest from file: " + err);
        }
    } else {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(from_string);
        } catch (const nlohmann::json::parse_error& e) {
            return make_error_response(
                "context",
                std::string("failed to parse manifest string: ") + e.what());
        }
        auto err = manifest_from_json(j, manifest);
        if (!err.empty()) {
            return make_error_response(
                "context", "invalid manifest string: " + err);
        }
    }

    // Check index availability
    if (indexer.is_indexing()) {
        return make_error_response("context",
                                   "index not available: indexing in progress");
    }

    // Parse filter/exclude
    std::vector<std::string> filter_roles;
    std::vector<std::string> exclude_roles;
    if (params.contains("filter") && params["filter"].is_array()) {
        for (const auto& f : params["filter"]) {
            if (f.is_string()) filter_roles.push_back(f.get<std::string>());
        }
    }
    if (params.contains("exclude") && params["exclude"].is_array()) {
        for (const auto& e : params["exclude"]) {
            if (e.is_string()) exclude_roles.push_back(e.get<std::string>());
        }
    }

    auto filtered_refs =
        filter_refs_by_role(manifest.refs, filter_roles, exclude_roles);

    int max_tokens = params.value("max_tokens", 0);
    if (max_tokens <= 0) {
        max_tokens = std::numeric_limits<int>::max();
    }

    // Hydrate
    HydratedContext result;
    result.task = manifest.task;
    int total_tokens = 0;

    ExpansionEngine engine(indexer);

    for (const auto& ref : filtered_refs) {
        if (total_tokens >= max_tokens) {
            result.warnings.push_back(
                "Truncated: reached token limit of " +
                std::to_string(max_tokens));
            result.stats.truncated = true;
            break;
        }

        auto hr = engine.hydrate_reference(ref, format, project_root);
        if (!hr.error.empty()) {
            result.warnings.push_back("Failed to hydrate " + ref.file + ":" +
                                      ref.symbol + ": " + hr.error);
            continue;
        }

        total_tokens += hr.tokens;
        result.stats.refs_loaded++;
        result.stats.symbols_hydrated++;

        // Apply expansions
        if (!ref.expansions.empty()) {
            auto exp_result = engine.apply_expansions(
                ref, hr.ref, format, max_tokens - total_tokens, project_root);
            if (!exp_result.error.empty()) {
                result.warnings.push_back("Failed to expand " + ref.file +
                                          ":" + ref.symbol + ": " +
                                          exp_result.error);
            } else {
                total_tokens += exp_result.tokens;
                result.stats.expansions_applied +=
                    static_cast<int>(ref.expansions.size());
            }
        }

        result.refs.push_back(std::move(hr.ref));
    }

    result.stats.tokens_approx = total_tokens;

    return make_json_response(hydrated_context_to_json(result));
}

}  // namespace

// -- handle_context -----------------------------------------------------------

ToolResult handle_context(const nlohmann::json& params,
                          MasterIndex& indexer,
                          const std::string& project_root) {
    auto operation = params.value("operation", "");

    if (operation == "save") {
        return handle_context_save(params, project_root);
    }
    if (operation == "load") {
        return handle_context_load(params, indexer, project_root);
    }

    return make_error_response(
        "context",
        "invalid operation: " + operation + " (must be 'save' or 'load')");
}

// -- register_context_handlers ------------------------------------------------

void register_context_handlers(McpServer& server, MasterIndex* indexer) {
    // Find and replace the stub "context" tool handler
    // The tool definition is already registered in register_tools()
    auto root = server.project_root();
    server.add_tool(
        {"context",
         "Capture and hydrate code context manifests for agent handoff. "
         "Save compact symbol references, load for instant full context.",
         {{"operation", "string", "Operation: 'save' or 'load'", ""},
          {"refs", "array",
           "References to save: [{f:'file', s:'symbol', l:{start,end}}]",
           ""},
          {"task", "string", "Task description for the manifest", ""},
          {"to_file", "string",
           "File path to save manifest (relative to project root)", ""},
          {"to_string", "boolean", "Return manifest as string instead", ""},
          {"append", "boolean", "Append to existing manifest file", ""},
          {"from_file", "string", "Load manifest from file", ""},
          {"from_string", "string", "Load manifest from JSON string", ""},
          {"format", "string",
           "Output format: 'full', 'signatures', 'outline'", ""},
          {"filter", "array", "Include only these roles", "string"},
          {"exclude", "array", "Exclude these roles", "string"},
          {"max_tokens", "integer",
           "Approximate token limit for hydrated context", ""}},
         {"operation"}},
        [indexer, root](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_error_response("context", "index not available");
            }
            return handle_context(p, *indexer, root);
        });
}

}  // namespace mcp
}  // namespace lci
