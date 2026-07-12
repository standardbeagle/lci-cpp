#include <lci/server/server.h>

#include <lci/indexing/master_index.h>
#include <lci/search/search_options.h>
#include <lci/server/request_decode.h>

#include <shared_mutex>

namespace lci {

// -- Endpoint: /search --------------------------------------------------------

void IndexServer::handle_search(const httplib::Request& req,
                                 httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    std::string decode_error;
    auto request = server_request::decode_search(body, decode_error);
    if (!request) {
        error_response(res, 400, decode_error);
        return;
    }

    // Fail fast on a path scope that matches NO indexed file. The CLI resolves
    // the positional to root-relative before sending, but a path that exists on
    // disk yet was never indexed (gitignored, wrong extension, outside the
    // root) would otherwise slip past to the path-scope filter, empty the
    // candidate set, and return an empty success (silent empty). Reject it
    // loudly instead. Reuses the existing `error` field which Client::search
    // already surfaces to the CLI as a nonzero-exit error.
    if (!request->paths.empty()) {
        std::vector<std::string> unmatched;
        {
            std::shared_lock lock(mu_);
            unmatched = indexer_->scopes_without_indexed_match(request->paths);
        }
        if (!unmatched.empty()) {
            std::string joined;
            for (size_t i = 0; i < unmatched.size(); ++i) {
                if (i) joined += ", ";
                joined += unmatched[i];
            }
            nlohmann::json j;
            j["error"] = "path matches no indexed file: " + joined;
            json_response(res, j);
            return;
        }
    }

    SearchOptions opts;
    opts.max_results = request->max_results;
    opts.case_insensitive = request->case_insensitive;
    opts.declaration_only = request->declaration_only;
    // Go's /search ships a `context` block per hit. Request a small
    // surrounding window so extract_context() actually populates lines;
    // without this max_context_lines=0 produces an empty `lines` array.
    opts.max_context_lines = request->max_context_lines;
    // Trailing CLI path args → index-side path scope (exact file or dir prefix,
    // OR across entries). Empty leaves search unscoped (unchanged behavior).
    opts.path_scopes = request->paths;

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_with_options(request->pattern, opts);
    }

    int max_res = opts.max_results;
    if (static_cast<int>(results.size()) > max_res) {
        results.resize(static_cast<size_t>(max_res));
    }

    nlohmann::json j;
    j["results"] = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json rj;
        rj["file_id"] = static_cast<int>(r.file_id);
        rj["path"] = r.path;
        rj["line"] = r.line;
        rj["column"] = r.column;
        rj["match"] = r.match_text;
        rj["score"] = r.score;

        // Build the structured `context` block Go's /search emits. The
        // search engine populates `context.lines` for the surrounding
        // window and the matched line number; the rest of the fields are
        // derived from those plus the SearchContext metadata.
        nlohmann::json ctx;
        ctx["block_type"] = r.context.block_type.empty()
                                ? std::string("lines")
                                : r.context.block_type;
        // `block_name` is the enclosing function/class/struct identifier when
        // the search engine could resolve one for the match. Always emit the
        // key (empty string when no enclosing block) so CLI consumers can rely
        // on the field being present and the JSON shape stays stable across
        // matches with and without semantic context.
        ctx["block_name"] = r.context.block_name;
        ctx["start_line"] = r.context.start_line;
        ctx["end_line"] = r.context.end_line;
        ctx["is_complete"] = r.context.is_complete;

        nlohmann::json lines_arr = nlohmann::json::array();
        for (const auto& l : r.context.lines) {
            lines_arr.push_back(l);
        }
        ctx["lines"] = lines_arr;

        nlohmann::json matched = nlohmann::json::array();
        matched.push_back(r.line);
        ctx["matched_lines"] = matched;
        ctx["match_count"] = 1;

        rj["context"] = ctx;
        j["results"].push_back(rj);
    }
    json_response(res, j);
}

// -- Endpoint: /definition ----------------------------------------------------

void IndexServer::handle_definition(const httplib::Request& req,
                                     httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    std::string decode_error;
    auto request = server_request::decode_limited_pattern(body, decode_error);
    if (!request) {
        error_response(res, 400, decode_error);
        return;
    }

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_definitions(request->pattern);
    }

    if (static_cast<int>(results.size()) > request->max_results) {
        results.resize(static_cast<size_t>(request->max_results));
    }

    nlohmann::json defs = nlohmann::json::array();
    for (const auto& r : results) {
        std::string name = r.context.block_name.empty()
                               ? r.match_text
                               : r.context.block_name;
        nlohmann::json d;
        d["name"] = name;
        d["type"] = r.context.block_type;
        d["file_path"] = r.path;
        d["line"] = r.line;
        d["column"] = r.column;
        defs.push_back(d);
    }

    nlohmann::json j;
    j["definitions"] = defs;
    json_response(res, j);
}

// -- Endpoint: /references ----------------------------------------------------

void IndexServer::handle_references(const httplib::Request& req,
                                     httplib::Response& res) {
    if (!require_ready(res)) return;

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception&) {
        error_response(res, 400, "invalid JSON body");
        return;
    }

    std::string decode_error;
    auto request = server_request::decode_limited_pattern(body, decode_error);
    if (!request) {
        error_response(res, 400, decode_error);
        return;
    }

    // Go's /references endpoint returns one entry per text occurrence of
    // the pattern (matching the Go reference output: a definition-line
    // hit in each file). Use the general search path so the C++ output
    // matches the Go shape and result count rather than restricting to
    // recorded incoming refs (which only covers cross-language linker
    // hits and would drop the same-file definition lines).
    SearchOptions opts;
    opts.max_results = request->max_results;
    opts.max_context_lines = 5;

    std::vector<SearchResult> results;
    {
        std::shared_lock lock(mu_);
        results = indexer_->search_with_options(request->pattern, opts);
    }

    if (static_cast<int>(results.size()) > request->max_results) {
        results.resize(static_cast<size_t>(request->max_results));
    }

    nlohmann::json refs = nlohmann::json::array();
    for (const auto& r : results) {
        std::string ctx;
        if (!r.context.lines.empty()) {
            int line_idx = r.line - r.context.start_line;
            if (line_idx >= 0 &&
                line_idx < static_cast<int>(r.context.lines.size())) {
                ctx = r.context.lines[static_cast<size_t>(line_idx)];
            } else {
                ctx = r.context.lines[0];
            }
        }

        nlohmann::json rj;
        rj["file_path"] = r.path;
        rj["line"] = r.line;
        rj["column"] = r.column;
        rj["context"] = ctx;
        rj["match"] = r.match_text;
        refs.push_back(rj);
    }

    nlohmann::json j;
    j["references"] = refs;
    json_response(res, j);
}

}  // namespace lci
