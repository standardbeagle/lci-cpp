#include <lci/mcp/handlers_context.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include <lci/indexing/master_index.h>
#include <lci/mcp/context_manifest_expander.h>
#include <lci/mcp/time_format.h>

namespace lci {
namespace mcp {

namespace {

/// Emits a one-line stderr warning the first time a verbose-shape key is
/// accepted on the load path. Karpathy rule 6 (no silent fallback): the
/// Go-shape compact keys are the contract; verbose keys are a transitional
/// accommodation that must surface.
void warn_verbose_key_once(const char* key, const char* compact) {
    std::cerr << "lci: warning: context_manifest accepted verbose key '"
              << key << "' (compact form is '" << compact
              << "' per Go reference internal/types/context_manifest_types.go"
                 " — DART-2PPeRKfyrceR)\n";
}

/// Reads a string field from a JSON object honouring a compact key (Go
/// reference) with an optional verbose-key fallback. Returns true if any
/// form was found and assigns to `out`. Emits a stderr warning on verbose
/// hits.
bool read_string_keyed(const nlohmann::json& j, const char* compact,
                       const char* verbose, std::string& out) {
    if (j.contains(compact) && j[compact].is_string()) {
        out = j[compact].get<std::string>();
        return true;
    }
    if (verbose && j.contains(verbose) && j[verbose].is_string()) {
        warn_verbose_key_once(verbose, compact);
        out = j[verbose].get<std::string>();
        return true;
    }
    return false;
}

}  // namespace

// -- JSON serialization -------------------------------------------------------

// Emits the Go-shape compact manifest body (matches internal/types/
// context_manifest_types.go json tags):
//   top-level: t (task), c (created RFC3339Nano), v (version), p (project_root),
//              r (refs), s (stats)
//   ref:       f, s, l:{s,e}, role, n (note), x
//   stats:     rc, tl, fc, rb
// The compact-key wire shape is locked by the context_manifest integration
// golden + the manifest round-trip unit tests.
nlohmann::json manifest_to_json(const ContextManifest& m) {
    nlohmann::json j;
    if (!m.task.empty()) j["t"] = m.task;
    // Created timestamp — Go's MarshalJSON does not stamp `c` (it is a passed-
    // through field), but emitting it on save is harmless because Go's
    // ContextManifest.Created uses `omitempty` on unmarshal. Stamp it so
    // round-trips through C++ preserve a creation time when none was set.
    j["c"] = format_rfc3339_nano_local(std::chrono::system_clock::now());
    // Go sets v="1.0" in MarshalJSON; mirror that default if caller did not
    // supply one explicitly.
    j["v"] = m.version.empty() ? std::string{"1.0"} : m.version;
    if (!m.project_root.empty()) j["p"] = m.project_root;

    auto& refs = j["r"];
    refs = nlohmann::json::array();
    refs.get_ref<nlohmann::json::array_t&>().reserve(m.refs.size());
    for (const auto& r : m.refs) {
        nlohmann::json rj;
        rj["f"] = r.file;
        if (!r.symbol.empty()) rj["s"] = r.symbol;
        if (r.has_line_range) {
            rj["l"] = {{"s", r.line_range.start},
                       {"e", r.line_range.end}};
        }
        if (!r.role.empty()) rj["role"] = r.role;
        if (!r.note.empty()) rj["n"] = r.note;
        if (!r.expansions.empty()) rj["x"] = r.expansions;
        refs.push_back(std::move(rj));
    }

    // Stats block — Go MarshalJSON calls ComputeStats(); mirror.
    auto stats = compute_manifest_stats(m);
    nlohmann::json sj;
    sj["rc"] = stats.ref_count;
    if (stats.total_lines > 0) sj["tl"] = stats.total_lines;
    if (stats.file_count > 0) sj["fc"] = stats.file_count;
    // Role breakdown — Go emits when non-empty.
    nlohmann::json rb = nlohmann::json::object();
    for (const auto& r : m.refs) {
        if (r.role.empty()) continue;
        if (rb.contains(r.role)) {
            rb[r.role] = rb[r.role].get<int>() + 1;
        } else {
            rb[r.role] = 1;
        }
    }
    if (!rb.empty()) sj["rb"] = std::move(rb);
    j["s"] = std::move(sj);

    return j;
}

// Accepts Go-shape compact keys as primary. Verbose keys (task/version/
// project_root/refs/note/start/end) are accepted as a transitional fallback
// with a one-line stderr warning per occurrence — karpathy rule 6, no silent
// fallback. The compact contract is enforced by the parity descriptors
// save-compact-keys.parity.json and save-verbose-keys-rejected.parity.json
// (the latter targets the *ref* keys f/s, which never fall back).
//
// Type handling:
//  - A top-level scalar (t/v/p) present but not a string is a fatal error: a
//    wrong-typed version must never be read as "absent" and default to 1.0.
//  - A per-ref selector of the wrong type (f/s/role/note, or a line-range
//    bound that is not an integer) is isolated as an invalid_ref unresolved
//    entry carrying the selectors that WERE well-typed, so a single bad ref
//    never aborts the load and never silently widens a file-scoped identity
//    into a global symbol-only lookup.
std::string manifest_from_json(const nlohmann::json& j, ContextManifest& out,
                               std::vector<UnresolvedRef>& invalid_out) {
    out = {};
    invalid_out = {};

    auto check_top_string = [&](const char* compact,
                                const char* verbose) -> std::string {
        if ((j.contains(compact) && !j[compact].is_string()) ||
            (verbose && j.contains(verbose) && !j[verbose].is_string())) {
            return std::string("manifest field '") + compact +
                   "' must be a string";
        }
        return {};
    };
    if (auto e = check_top_string("t", "task"); !e.empty()) return e;
    if (auto e = check_top_string("v", "version"); !e.empty()) return e;
    if (auto e = check_top_string("p", "project_root"); !e.empty()) return e;

    read_string_keyed(j, "t", "task", out.task);
    read_string_keyed(j, "v", "version", out.version);
    read_string_keyed(j, "p", "project_root", out.project_root);

    // Refs array: Go uses `r`; accept verbose `refs` as load-only fallback.
    const nlohmann::json* refs_ptr = nullptr;
    if (j.contains("r") && j["r"].is_array()) {
        refs_ptr = &j["r"];
    } else if (j.contains("refs") && j["refs"].is_array()) {
        warn_verbose_key_once("refs", "r");
        refs_ptr = &j["refs"];
    } else {
        return "missing or invalid 'r' (refs) array";
    }

    for (const auto& rj : *refs_ptr) {
        if (!rj.is_object()) {
            UnresolvedRef bad;
            bad.reason = RefResolution::InvalidRef;
            invalid_out.push_back(std::move(bad));
            continue;
        }
        // Well-typed string selectors are retained for the unresolved report;
        // a present-but-wrong-typed selector invalidates the whole ref.
        auto opt_string = [&](const char* k) -> std::optional<std::string> {
            if (!rj.contains(k)) return std::nullopt;
            if (!rj[k].is_string()) return std::nullopt;  // signalled below
            return rj[k].get<std::string>();
        };
        const bool f_bad = rj.contains("f") && !rj["f"].is_string();
        const bool s_bad = rj.contains("s") && !rj["s"].is_string();
        const bool role_bad = rj.contains("role") && !rj["role"].is_string();
        // note: compact `n` or verbose `note` must both be strings if present.
        const bool note_bad =
            (rj.contains("n") && !rj["n"].is_string()) ||
            (rj.contains("note") && !rj["note"].is_string());
        // line range: if `l` present it must be an object; its s/e (or
        // start/end) bounds, when present, must be integers.
        bool lines_bad = false;
        bool has_lines = false;
        int lstart = 0, lend = 0;
        if (rj.contains("l")) {
            if (!rj["l"].is_object()) {
                lines_bad = true;
            } else {
                const auto& lj = rj["l"];
                auto num = [&](const char* k, const char* vk, int& dst) -> bool {
                    const char* use = lj.contains(k) ? k
                                : (lj.contains(vk) ? vk : nullptr);
                    if (!use) return true;
                    if (!lj[use].is_number_integer()) return false;
                    dst = lj[use].get<int>();
                    return true;
                };
                // A `l` object must carry at least one honoured integer bound
                // (s/start or e/end). A bound-less `l: {}` yields no valid line
                // range, so it must not be treated as an honoured selector.
                has_lines = lj.contains("s") || lj.contains("start") ||
                            lj.contains("e") || lj.contains("end");
                if (!num("s", "start", lstart) || !num("e", "end", lend)) {
                    lines_bad = true;
                }
            }
        }

        if (f_bad || s_bad || role_bad || note_bad || lines_bad) {
            UnresolvedRef bad;
            bad.reason = RefResolution::InvalidRef;
            if (auto fv = opt_string("f")) bad.file = std::move(*fv);
            if (auto sv = opt_string("s")) bad.symbol = std::move(*sv);
            if (auto rv = opt_string("role")) bad.role = std::move(*rv);
            if (auto nv = opt_string("n"))
                bad.note = std::move(*nv);
            else if (auto nv = opt_string("note"))
                bad.note = std::move(*nv);
            if (has_lines && !lines_bad) {
                bad.lines = {lstart, lend};
                bad.has_line_range = true;
            }
            invalid_out.push_back(std::move(bad));
            continue;
        }

        ContextRef r;
        // f/s are compact-only by contract; no verbose alias here — the
        // negative parity descriptor save-verbose-keys-rejected.parity.json
        // locks rejection of {file, symbol} on the ref level.
        if (rj.contains("f") && rj["f"].is_string()) {
            r.file = rj["f"].get<std::string>();
        }
        if (rj.contains("s") && rj["s"].is_string()) {
            r.symbol = rj["s"].get<std::string>();
        }
        if (has_lines) {
            r.line_range = {lstart, lend};
            r.has_line_range = true;
        }
        if (rj.contains("role") && rj["role"].is_string()) {
            r.role = rj["role"].get<std::string>();
        }
        // note: Go uses `n`; accept verbose `note` as load-only fallback.
        read_string_keyed(rj, "n", "note", r.note);
        if (rj.contains("x") && rj["x"].is_array()) {
            for (const auto& x : rj["x"]) {
                if (x.is_string()) {
                    r.expansions.push_back(x.get<std::string>());
                }
            }
        }
        // A ref must carry at least one honoured selector: a compact `f`, a
        // compact `s`, or a valid line range. A ref whose only selector keys
        // are verbose `file`/`symbol` (never honoured at the ref level — see
        // save-verbose-keys-rejected.parity.json) and a bare `{}` carry none.
        // Such a ref is an invalid_ref, never an empty accepted ref: it must
        // not enter refs nor count as a resolved ref, and it must not be left
        // for hydrate to mask (which would silently drop the raw selectors).
        // Preserve the well-typed raw selector strings (compact, else verbose),
        // role and note in the unresolved report.
        if (r.file.empty() && r.symbol.empty() && !r.has_line_range) {
            UnresolvedRef bad;
            bad.reason = RefResolution::InvalidRef;
            bad.file = r.file;
            bad.symbol = r.symbol;
            bad.role = r.role;
            bad.note = r.note;
            if (bad.file.empty()) {
                if (auto v = opt_string("file")) bad.file = std::move(*v);
            }
            if (bad.symbol.empty()) {
                if (auto v = opt_string("symbol")) bad.symbol = std::move(*v);
            }
            invalid_out.push_back(std::move(bad));
            continue;
        }
        out.refs.push_back(std::move(r));
    }

    return {};
}

std::string manifest_from_json(const nlohmann::json& j, ContextManifest& out) {
    std::vector<UnresolvedRef> ignored;
    return manifest_from_json(j, out, ignored);
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

/// Serializes one hydrated ref to its JSON object. Shared by the whole-context
/// serializer and the budget admission loop, which sizes each candidate by
/// dumping just this object.
nlohmann::json hydrated_ref_to_json(const HydratedRef& r) {
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
    if (r.is_line_range_literal) rj["line_range_literal"] = true;
    // Every requested role / relationship that collapsed onto this canonical
    // identity survives here, so dedup never loses provenance.
    if (!r.provenance.empty()) rj["provenance"] = r.provenance;
    return rj;
}

nlohmann::json hydrated_context_to_json(const HydratedContext& ctx) {
    nlohmann::json j;
    if (!ctx.task.empty()) j["task"] = ctx.task;

    auto& refs = j["refs"];
    refs = nlohmann::json::array();
    refs.get_ref<nlohmann::json::array_t&>().reserve(ctx.refs.size());
    for (const auto& r : ctx.refs) {
        refs.push_back(hydrated_ref_to_json(r));
    }

    // Structured unresolved entries: the original selector + role + reason,
    // so a caller can act on what failed without parsing warning prose.
    if (!ctx.unresolved.empty()) {
        auto& un = j["unresolved"];
        un = nlohmann::json::array();
        for (const auto& u : ctx.unresolved) {
            nlohmann::json uj;
            uj["file"] = u.file;
            if (!u.symbol.empty()) uj["symbol"] = u.symbol;
            if (!u.role.empty()) uj["role"] = u.role;
            if (!u.note.empty()) uj["note"] = u.note;
            if (u.has_line_range) {
                uj["lines"] = {{"start", u.lines.start}, {"end", u.lines.end}};
            }
            uj["reason"] = to_string(u.reason);
            un.push_back(std::move(uj));
        }
    }

    j["stats"] = {{"refs_loaded", ctx.stats.refs_loaded},
                  {"symbols_hydrated", ctx.stats.symbols_hydrated},
                  {"tokens_approx", ctx.stats.tokens_approx},
                  {"expansions_applied", ctx.stats.expansions_applied},
                  {"unresolved_count", ctx.stats.unresolved_count},
                  {"refs_omitted", ctx.stats.refs_omitted},
                  {"expansions_omitted", ctx.stats.expansions_omitted},
                  {"traversal_truncated", ctx.stats.traversal_truncated},
                  {"truncated", ctx.stats.truncated}};

    if (!ctx.warnings.empty()) j["warnings"] = ctx.warnings;

    return j;
}

// -- Internal helpers ---------------------------------------------------------

namespace {

/// Resolves a manifest path against the project root and confines it there.
/// Returns the resolved absolute path, or empty when the input (absolute
/// path, `..` traversal, symlink) lands outside the project root — callers
/// answer with an error, never touch the filesystem outside the root.
std::string resolve_manifest_path(const std::string& relative_path,
                                   const std::string& project_root) {
    namespace fs = std::filesystem;
    if (relative_path.empty()) return {};

    std::error_code ec;
    auto root = fs::weakly_canonical(
        project_root.empty() ? fs::path(".") : fs::path(project_root), ec);
    if (ec || root.empty()) return {};

    fs::path candidate(relative_path);
    if (!candidate.is_absolute()) candidate = root / candidate;
    // weakly_canonical resolves `..`, `.`, and existing symlinks without
    // requiring the file to exist yet (save creates it).
    auto resolved = fs::weakly_canonical(candidate, ec);
    if (ec) return {};

    // Containment: resolved must equal root or live beneath it.
    auto root_str = root.generic_string();
    auto resolved_str = resolved.generic_string();
    if (resolved_str.size() <= root_str.size() ||
        resolved_str.compare(0, root_str.size(), root_str) != 0 ||
        resolved_str[root_str.size()] != '/') {
        return {};
    }
    return resolved.string();
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
    // Lossy dump: ref notes / tasks / file paths are caller- and
    // source-derived text that may hold non-UTF-8 bytes (e.g. 0x8A latin-1);
    // the strict default handler throws type_error.316 and fails the whole
    // save. Same rationale as dump_json_lossy (response.cpp), with indent.
    auto json_str = data.dump(2, ' ', /*ensure_ascii=*/false,
                              nlohmann::json::error_handler_t::replace);

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

/// Loads a manifest from a file. Fatal for top-level/parse problems; a
/// per-ref selector of the wrong type is isolated into `invalid_out` (the load
/// path) so it does not abort the whole manifest. Returns a fatal error only
/// when nothing at all could be parsed.
std::string load_manifest_from_file(const std::string& file_path,
                                    ContextManifest& out,
                                    std::vector<UnresolvedRef>& invalid_out) {
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

    auto err = manifest_from_json(j, out, invalid_out);
    if (!err.empty()) return err;
    if (out.refs.empty() && invalid_out.empty()) {
        return "manifest must have at least one reference";
    }
    return {};
}

/// Append-oriented loader: refuses any manifest it cannot round-trip losslessly
/// (a top-level parse problem, or any ref the shared parser isolated as
/// invalid). The caller's merge-and-overwrite path would otherwise silently
/// erase the isolated refs, so this contract is stricter than the three-arg
/// loader's per-ref isolation used on the load path. `validate_manifest` still
/// catches refs that survived parse but carry no honoured selector.
std::string load_manifest_from_file(const std::string& file_path,
                                    ContextManifest& out) {
    std::vector<UnresolvedRef> invalid;
    auto err = load_manifest_from_file(file_path, out, invalid);
    if (!err.empty()) return err;
    if (!invalid.empty()) {
        return "contains " + std::to_string(invalid.size()) +
               " unparseable reference(s) that an append would silently drop";
    }
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

    // Build manifest. The save tool's input schema is {task, refs:[{f,s,l,
    // role,n,x}], ...} on the params object — the `refs` argument array is
    // the canonical input shape, distinct from a stored manifest body (which
    // uses the Go-shape `r` key and is parsed by manifest_from_json on load).
    // Parse the refs argument directly; ref keys are Go-shape compact
    // f/s/l{s,e}/n with verbose start/end/note tolerated for input ergonomics.
    ContextManifest manifest;
    manifest.task = params.value("task", "");
    manifest.project_root = project_root;
    // Parse each ref with explicit type checks. The refs argument is the
    // save tool's compact input shape {f, s, l{s,e}, role, n, x} (verbose
    // start/end/note tolerated for input ergonomics, like the load path).
    // A present-but-wrong-typed selector must fail the whole save with a
    // ref-indexed message rather than throw out of the handler: an append
    // that fails must leave the existing manifest file untouched, and an
    // uncaught nlohmann type_error would surface as a generic "Internal
    // error" (or abort the unit call) instead of an explicit rejection. A
    // selector-less ref (verbose-only, bare, or an all-empty ref) carries no
    // honoured f/s/l and is caught below by validate_manifest.
    const std::string prefix = "invalid manifest: ref[";
    for (size_t i = 0; i < params["refs"].size(); ++i) {
        const auto& rj = params["refs"][i];
        const std::string where = prefix + std::to_string(i) + "]";
        if (!rj.is_object()) {
            return make_error_response("context", where + " must be an object");
        }
        ContextRef r;
        auto read_str = [&](const char* k, std::string& dst) -> bool {
            if (!rj.contains(k)) return true;
            if (!rj[k].is_string()) {
                return false;
            }
            dst = rj[k].get<std::string>();
            return true;
        };
        if (!read_str("f", r.file) || !read_str("s", r.symbol) ||
            !read_str("role", r.role)) {
            return make_error_response(
                "context", where + " selector must be a string");
        }
        // note: compact `n` preferred, verbose `note` as input fallback
        // (matches the load-only aliasing in manifest_from_json).
        std::string note;
        if (!read_str("n", note) ||
            (note.empty() && !read_str("note", note))) {
            return make_error_response(
                "context", where + " note must be a string");
        }
        r.note = note;
        if (rj.contains("l")) {
            if (!rj["l"].is_object()) {
                return make_error_response(
                    "context", where + " line range must be an object");
            }
            const auto& lj = rj["l"];
            auto read_int = [&](const char* k, const char* vk,
                                int& dst) -> bool {
                const char* use = lj.contains(k) ? k
                              : (lj.contains(vk) ? vk : nullptr);
                if (!use) return true;
                if (!lj[use].is_number_integer()) return false;
                dst = lj[use].get<int>();
                return true;
            };
            LineRange lr;
            if (!read_int("s", "start", lr.start) ||
                !read_int("e", "end", lr.end)) {
                return make_error_response(
                    "context",
                    where + " line range bounds must be integers");
            }
            r.line_range = lr;
            r.has_line_range = true;
        }
        if (rj.contains("x") && rj["x"].is_array()) {
            for (const auto& x : rj["x"]) {
                if (x.is_string()) {
                    r.expansions.push_back(x.get<std::string>());
                }
            }
        }
        manifest.refs.push_back(std::move(r));
    }

    auto val_err = validate_manifest(manifest);
    if (!val_err.empty()) {
        return make_error_response("context", "invalid manifest: " + val_err);
    }

    // Handle append mode
    bool append = params.value("append", false);
    if (append && !to_file.empty()) {
        auto full_path = resolve_manifest_path(to_file, project_root);
        if (full_path.empty()) {
            return make_error_response(
                "context",
                "'to_file' must resolve inside the project root: " + to_file);
        }
        ContextManifest existing;
        std::error_code ec;
        if (std::filesystem::exists(full_path, ec) && !ec) {
            // The file exists but cannot be parsed/validated: refuse the
            // append instead of silently overwriting the manifest with only
            // the new refs (a corrupt manifest is a user problem to surface,
            // not to destroy).
            auto load_err = load_manifest_from_file(full_path, existing);
            if (!load_err.empty()) {
                return make_error_response(
                    "context",
                    "cannot append: existing manifest is unreadable: " +
                        load_err);
            }
            // Merge: prepend existing refs
            existing.refs.insert(existing.refs.end(),
                                 manifest.refs.begin(),
                                 manifest.refs.end());
            if (manifest.task.empty()) {
                manifest.task = existing.task;
            }
            manifest.refs = std::move(existing.refs);
        }
        // A missing file is fine for append — it behaves like a fresh save.
    }

    auto stats = compute_manifest_stats(manifest);

    if (!to_file.empty()) {
        auto full_path = resolve_manifest_path(to_file, project_root);
        if (full_path.empty()) {
            return make_error_response(
                "context",
                "'to_file' must resolve inside the project root: " + to_file);
        }
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
    // Lossy for the same reason as save_manifest_to_file: strict dump throws
    // type_error.316 on non-UTF-8 input bytes and fails the whole call.
    response["manifest"] =
        manifest_json.dump(2, ' ', /*ensure_ascii=*/false,
                           nlohmann::json::error_handler_t::replace);
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

    // Load manifest. A per-ref selector of the wrong type is isolated into
    // invalid_refs (reported unresolved, reason invalid_ref) rather than
    // aborting the whole load; only top-level problems are fatal.
    ContextManifest manifest;
    std::vector<UnresolvedRef> invalid_refs;
    if (!from_file.empty()) {
        auto full_path = resolve_manifest_path(from_file, project_root);
        if (full_path.empty()) {
            return make_error_response(
                "context",
                "'from_file' must resolve inside the project root: " +
                    from_file);
        }
        auto err = load_manifest_from_file(full_path, manifest, invalid_refs);
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
        auto err = manifest_from_json(j, manifest, invalid_refs);
        if (!err.empty()) {
            return make_error_response(
                "context", "invalid manifest string: " + err);
        }
    }

    // Version gate: a well-formed manifest whose schema version is neither
    // empty (default 1.0) nor "1.0" cannot be hydrated. Report every stored
    // ref as structured unresolved with reason unsupported_version (original
    // selectors and roles preserved) rather than a bare prose error, and keep
    // a legacy warning for continuity.
    if (!manifest.version.empty() && manifest.version != "1.0") {
        HydratedContext fail;
        fail.task = manifest.task;
        for (const auto& r : manifest.refs) {
            UnresolvedRef u;
            u.file = r.file;
            u.symbol = r.symbol;
            u.role = r.role;
            u.note = r.note;
            u.lines = r.line_range;
            u.has_line_range = r.has_line_range;
            u.reason = RefResolution::UnsupportedVersion;
            fail.unresolved.push_back(std::move(u));
        }
        for (auto& u : invalid_refs) {
            u.reason = RefResolution::UnsupportedVersion;
            fail.unresolved.push_back(std::move(u));
        }
        fail.stats.unresolved_count = static_cast<int>(fail.unresolved.size());
        fail.warnings.push_back(
            "unsupported manifest version '" + manifest.version +
            "' (supported: 1.0)");
        return make_json_response(hydrated_context_to_json(fail));
    }

    // Check index availability
    if (indexer.is_indexing()) {
        return make_unavailable_response(
        "context", "index not available: indexing in progress", "retry shortly; the server is still starting or indexing");
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
    const bool bounded = max_tokens > 0;
    const size_t byte_ceiling = bounded
        ? static_cast<size_t>(max_tokens) * 4
        : std::numeric_limits<size_t>::max();

    // Traversal input bound, applied to a bounded load only: a request naming
    // more refs than the documented cap is rejected as a typed error rather
    // than silently traversed. An unlimited load preserves legacy behavior.
    if (bounded &&
        filtered_refs.size() > static_cast<size_t>(kHydrationMaxInputRefs)) {
        nlohmann::json err;
        err["success"] = false;
        err["operation"] = "context";
        err["code"] = "too_many_refs";
        err["error"] = "a bounded load accepts at most " +
                       std::to_string(kHydrationMaxInputRefs) +
                       " input refs (got " +
                       std::to_string(filtered_refs.size()) +
                       "); set max_tokens to 0 for an unbounded load";
        return {dump_json_lossy(err), true};
    }

    // Parse-time invalid selectors surface as unresolved entries (preserving
    // their original selectors/roles) alongside any that fail hydration.
    std::vector<UnresolvedRef> unresolved = std::move(invalid_refs);

    ExpansionEngine engine(indexer);

    // Ordered working-set candidates. Identities are deduplicated across the
    // WHOLE set (primaries first, then every expansion target) BEFORE any
    // budget decision, so an identity's source is hydrated once and the budget
    // only chooses how much of that deduplicated set to admit. `is_expansion`
    // lets the emitted stats report primaries-omitted and expansions-omitted
    // separately.
    struct Candidate {
        HydratedRef hr;
        bool is_expansion{};
        size_t obj_bytes{};  // serialized bytes of just this ref object
    };
    std::vector<Candidate> candidates;
    candidates.reserve(filtered_refs.size());
    absl::flat_hash_set<std::string> emitted_keys;
    absl::flat_hash_map<std::string, size_t> key_to_index;
    std::vector<int> ref_to_candidate(filtered_refs.size(), -1);

    auto add_provenance = [](HydratedRef& hr, const std::string& label) {
        if (label.empty()) return;
        for (const auto& p : hr.provenance) {
            if (p == label) return;
        }
        hr.provenance.push_back(label);
    };

    // Register a hydrated ref under its canonical identity: a first sighting
    // becomes a candidate; a repeat merges its role/relationship into the
    // surviving entry's provenance without adding another source. Returns the
    // candidate index (new or existing).
    auto register_identity = [&](HydratedRef hr, bool is_expansion,
                                 const std::string& label) -> size_t {
        std::string key = engine.identity_key(hr, project_root);
        auto it = key_to_index.find(key);
        if (it != key_to_index.end()) {
            add_provenance(candidates[it->second].hr, label);
            return it->second;
        }
        size_t idx = candidates.size();
        add_provenance(hr, label);
        key_to_index.emplace(std::move(key), idx);
        Candidate cand;
        cand.hr = std::move(hr);
        cand.is_expansion = is_expansion;
        cand.obj_bytes = dump_json_lossy(hydrated_ref_to_json(cand.hr)).size();
        candidates.push_back(std::move(cand));
        return idx;
    };

    // Pass 1 — primaries, in manifest order. A ref that fails identity
    // resolution is an honest unresolved entry, never a silent drop.
    for (size_t i = 0; i < filtered_refs.size(); ++i) {
        const auto& ref = filtered_refs[i];
        auto hr = engine.hydrate_reference(ref, format, project_root);
        if (!hr.error.empty() || hr.reason != RefResolution::Resolved) {
            UnresolvedRef ue;
            ue.file = ref.file;
            ue.symbol = ref.symbol;
            ue.role = ref.role;
            ue.note = ref.note;
            ue.lines = ref.line_range;
            ue.has_line_range = ref.has_line_range;
            ue.reason = hr.reason == RefResolution::Resolved
                            ? RefResolution::InvalidRef
                            : hr.reason;
            unresolved.push_back(std::move(ue));
            continue;
        }
        ref_to_candidate[i] = static_cast<int>(
            register_identity(std::move(hr.ref), /*is_expansion=*/false,
                              ref.role));
    }

    // Pass 2 — expansions, in manifest order, admitted AFTER all primaries.
    // The shared tally dedups every target against the whole working set and
    // applies the independent traversal caps (depth, visited targets).
    ExpansionTally tally;
    tally.emitted_keys = &emitted_keys;
    tally.max_depth =
        bounded ? kHydrationMaxExpansionDepth : std::numeric_limits<int>::max();
    tally.visited_cap =
        bounded ? kHydrationMaxVisitedTargets : std::numeric_limits<int>::max();
    for (const auto& kv : key_to_index) {
        emitted_keys.insert(kv.first);
    }

    int expansions_applied = 0;
    for (size_t i = 0; i < filtered_refs.size(); ++i) {
        const auto& ref = filtered_refs[i];
        if (ref.expansions.empty()) continue;
        int primary_idx = ref_to_candidate[i];
        if (primary_idx < 0) continue;  // primary unresolved -> nothing to seed

        HydratedRef& primary = candidates[primary_idx].hr;
        size_t dedup_before = tally.deduped.size();
        auto exp =
            engine.apply_expansions(ref, primary, format, project_root, tally);
        expansions_applied += static_cast<int>(ref.expansions.size());

        // Newly-expanded identities (already deduplicated and registered in
        // emitted_keys by the shared tally) become expansion candidates in
        // encounter order, after every primary.
        for (auto& er : exp.expanded) {
            register_identity(std::move(er), /*is_expansion=*/true,
                              /*label=*/"");
        }
        // Targets whose identity already existed contribute a relationship to
        // the surviving entry's provenance, never a second copy of source.
        for (size_t d = dedup_before; d < tally.deduped.size(); ++d) {
            auto it = key_to_index.find(tally.deduped[d].first);
            if (it != key_to_index.end()) {
                add_provenance(candidates[it->second].hr,
                               tally.deduped[d].second);
            }
        }
    }

    // Build the final response under ONE shared serialized-JSON budget. The
    // charge is ceil(UTF-8 bytes of the full serialized context / 4) — an
    // estimate of wire size, not a model-tokenizer count.
    size_t total_primaries = 0, total_expansions = 0;
    for (const auto& c : candidates) {
        if (c.is_expansion) ++total_expansions;
        else ++total_primaries;
    }

    auto build_context = [&](const std::vector<size_t>& admitted) {
        HydratedContext ctx;
        ctx.task = manifest.task;
        ctx.unresolved = unresolved;
        absl::flat_hash_set<size_t> chosen(admitted.begin(), admitted.end());
        size_t loaded_primaries = 0, loaded_expansions = 0;
        for (const auto& idx : admitted) {
            const Candidate& c = candidates[idx];
            if (c.is_expansion) ++loaded_expansions;
            else ++loaded_primaries;
            ctx.refs.push_back(c.hr);
        }
        ctx.stats.refs_loaded = static_cast<int>(loaded_primaries);
        for (const auto& idx : admitted) {
            if (!candidates[idx].is_expansion &&
                !candidates[idx].hr.symbol.empty()) {
                ++ctx.stats.symbols_hydrated;
            }
        }
        ctx.stats.expansions_applied = expansions_applied;
        ctx.stats.unresolved_count = static_cast<int>(ctx.unresolved.size());
        ctx.stats.refs_omitted =
            static_cast<int>(total_primaries - loaded_primaries);
        ctx.stats.expansions_omitted =
            static_cast<int>(total_expansions - loaded_expansions);
        ctx.stats.traversal_truncated = tally.traversal_truncated;
        const bool truncated = ctx.stats.refs_omitted > 0 ||
                               ctx.stats.expansions_omitted > 0;
        ctx.stats.truncated = truncated;
        if (truncated) {
            ctx.warnings.push_back(
                "Context truncated to fit the max_tokens budget; omitted refs "
                "and expansions are reported in stats (estimates: not a model "
                "tokenizer count).");
        }
        if (tally.traversal_truncated) {
            ctx.warnings.push_back(
                "Expansion traversal bounded by the depth/visited-target caps.");
        }
        return ctx;
    };

    auto serialized_bytes = [](const HydratedContext& ctx) {
        return dump_json_lossy(hydrated_context_to_json(ctx)).size();
    };

    // Every index is a candidate to consider, in order (primaries first because
    // they were registered first).
    std::vector<size_t> all_indices;
    all_indices.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) all_indices.push_back(i);

    // Fast path: unbounded, or the entire deduplicated working set fits — emit
    // everything with no truncation.
    if (!bounded) {
        HydratedContext ctx = build_context(all_indices);
        ctx.stats.tokens_approx =
            hydration_token_estimate(serialized_bytes(ctx));
        return make_json_response(hydrated_context_to_json(ctx));
    }

    {
        HydratedContext probe = build_context(all_indices);
        probe.stats.tokens_approx = max_tokens;  // reserve estimate width
        if (serialized_bytes(probe) <= byte_ceiling) {
            HydratedContext ctx = build_context(all_indices);
            ctx.stats.tokens_approx =
                hydration_token_estimate(serialized_bytes(ctx));
            return make_json_response(hydrated_context_to_json(ctx));
        }
    }

    // Greedy admit in priority order (primaries then expansions), sized against
    // the real serialized envelope plus each candidate's own ref object. The
    // truncation-reporting frame is measured with zero refs so the minimal
    // response is known to fit before any content is added.
    HydratedContext frame = build_context({});
    frame.stats.tokens_approx = max_tokens;
    const size_t frame_bytes = serialized_bytes(frame);

    if (frame_bytes > byte_ceiling) {
        // Even the minimal (empty-content) response, which must still report
        // the omissions honestly, cannot fit. Fail with a typed error rather
        // than return an oversized or clipped payload.
        nlohmann::json err;
        err["success"] = false;
        err["operation"] = "context";
        err["code"] = "budget_too_small";
        err["error"] =
            "max_tokens " + std::to_string(max_tokens) +
            " cannot fit the minimal context envelope; the smallest valid "
            "response estimates " +
            std::to_string(hydration_token_estimate(frame_bytes)) +
            " tokens";
        return {dump_json_lossy(err), true};
    }

    std::vector<size_t> admitted;
    size_t running = frame_bytes;
    for (size_t idx : all_indices) {
        const size_t cost =
            candidates[idx].obj_bytes + (admitted.empty() ? 0 : 1);
        if (running + cost > byte_ceiling) break;  // this and later are omitted
        admitted.push_back(idx);
        running += cost;
    }

    // Final exact serialize, then shrink the tail (lowest-priority, i.e. the
    // last-admitted expansion, then primary) until the reported estimate fits.
    while (true) {
        HydratedContext ctx = build_context(admitted);
        ctx.stats.tokens_approx = hydration_token_estimate(serialized_bytes(ctx));
        if (serialized_bytes(ctx) <= byte_ceiling) {
            return make_json_response(hydrated_context_to_json(ctx));
        }
        if (admitted.empty()) {
            // Should be unreachable (frame fit above), but never emit an
            // oversized payload.
            nlohmann::json err;
            err["success"] = false;
            err["operation"] = "context";
            err["code"] = "budget_too_small";
            err["error"] = "max_tokens " + std::to_string(max_tokens) +
                           " cannot fit the minimal context envelope";
            return {dump_json_lossy(err), true};
        }
        admitted.pop_back();
    }
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
    // Registers the "context" tool (definition + real handler) — the sole
    // registration of this tool now that the stub registrar is gone.
    auto root = server.project_root();

    // Build context tool definition. The `refs` property has a complex
    // nested-object items schema for Go parity (jsonschema-go emits the full
    // ref struct schema with f/l/note/role/s/x fields). All other properties
    // are flat. Per-property key order inside refs.items.properties is
    // alphabetical (matches Go map iteration).
    ToolDefinition ctx_def;
    ctx_def.name = "context";
    ctx_def.description =
        "🎯 Capture and hydrate code context manifests for efficient agent "
        "handoff. Save compact symbol references (2-5KB manifest), load to "
        "get instant full context with source code + call graphs. "
        "Eliminates redundant exploration across agent sessions. "
        "Operations: 'save' to create manifest, 'load' to hydrate. See "
        "'info context'.";

    ctx_def.properties = {
        {"operation", "string",
         "Operation: 'save' to create manifest, 'load' to hydrate context",
         "", {}},
        {"refs", "array", "Code references to save (for 'save' operation)",
         "", {}},  // items_schema_override set below
        {"task", "string", "Task description/directive (free-form text)", "",
         {}},
        {"to_file", "string",
         "Write manifest to file path (relative to project root)", "", {}},
        {"to_string", "boolean",
         "Return manifest as JSON string instead of writing to file", "",
         {}},
        {"append", "boolean", "Append to existing manifest (default: false)",
         "", {}},
        {"from_file", "string",
         "Load manifest from file path (for 'load' operation)", "", {}},
        {"from_string", "string",
         "Load manifest from inline JSON string", "", {}},
        {"format", "string",
         "Output format: 'full' (default), 'signatures', 'outline'", "", {}},
        {"filter", "array",
         "Only include these roles (e.g., ['modify', 'contract'])",
         "string", {}},
        {"exclude", "array", "Exclude these roles", "string", {}},
        {"max_tokens", "integer",
         "Approximate token limit for hydrated context (0 = no limit)", "",
         {}},
    };

    // Set the nested-object items schema for `refs`. Property keys inside
    // items.properties emit alphabetically; per-property keys emit type
    // then description (matches Go jsonschema-go Property struct order).
    // Use ordered_json so insertion order is preserved on dump.
    nlohmann::ordered_json refs_items;
    refs_items["properties"] = nlohmann::ordered_json::object();
    {
        auto& props = refs_items["properties"];

        nlohmann::ordered_json f;
        f["type"] = "string";
        f["description"] = "File path (required)";
        props["f"] = std::move(f);

        nlohmann::ordered_json l;
        l["properties"] = nlohmann::ordered_json::object();
        {
            nlohmann::ordered_json end_p;
            end_p["type"] = "integer";
            end_p["description"] = "End line (1-indexed)";
            l["properties"]["e"] = std::move(end_p);

            nlohmann::ordered_json start_p;
            start_p["type"] = "integer";
            start_p["description"] = "Start line (1-indexed)";
            l["properties"]["s"] = std::move(start_p);
        }
        l["type"] = "object";
        l["description"] = "Line range {s, e} (optional)";
        props["l"] = std::move(l);

        nlohmann::ordered_json note;
        note["type"] = "string";
        note["description"] = "Architect annotation (free-form text)";
        props["n"] = std::move(note);

        nlohmann::ordered_json role;
        role["type"] = "string";
        role["description"] =
            "Semantic role: 'modify', 'contract', 'pattern', 'boundary'";
        props["role"] = std::move(role);

        nlohmann::ordered_json s;
        s["type"] = "string";
        s["description"] = "Symbol name (optional)";
        props["s"] = std::move(s);

        nlohmann::ordered_json x;
        x["type"] = "array";
        x["description"] =
            "Expansion directives: 'callers', 'callees:2' (with purity "
            "info), 'implementations', 'tests', etc.";
        x["items"] = nlohmann::ordered_json::object();
        x["items"]["type"] = "string";
        props["x"] = std::move(x);
    }
    refs_items["required"] = nlohmann::ordered_json::array();
    refs_items["required"].push_back("f");
    refs_items["type"] = "object";
    // Find refs property and attach the override. Convert ordered_json to
    // nlohmann::json for storage; serialization back through ordered_json
    // in build_input_schema_ordered() preserves the order via dump+parse.
    for (auto& prop : ctx_def.properties) {
        if (prop.name == "refs") {
            prop.items_schema_override =
                nlohmann::json::parse(refs_items.dump());
            break;
        }
    }

    ctx_def.required = {"operation"};

    server.add_tool(
        std::move(ctx_def),
        [indexer, root](const nlohmann::json& p) -> ToolResult {
            if (!indexer) {
                return make_unavailable_response("context", "index not available",
                                                 "retry shortly; the server is still starting or indexing");
            }
            return handle_context(p, *indexer, root);
        }
        // exclusive (default): save/append write manifest files (read-modify-
        // write on append); not a read-only tool.
    );
}

}  // namespace mcp
}  // namespace lci
