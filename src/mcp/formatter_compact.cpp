#include <lci/mcp/formatter_compact.h>

#include <sstream>

namespace lci {
namespace mcp {

// -- CompactFormatter: search -------------------------------------------------

std::string CompactFormatter::format_search_response(
    const nlohmann::json& response) const {
    std::vector<std::string> lines;
    lines.push_back("LCF/1.0");

    int total = response.value("total_matches", 0);
    int showing = response.value("showing", 0);
    int max_results = response.value("max_results", 0);
    lines.push_back("total=" + std::to_string(total) +
                    " showing=" + std::to_string(showing) +
                    " max=" + std::to_string(max_results));

    if (response.contains("results") && response["results"].is_array()) {
        for (const auto& r : response["results"]) {
            lines.push_back(format_search_result(r));
        }
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string CompactFormatter::format_files_only_response(
    const nlohmann::json& response) const {
    std::vector<std::string> lines;
    lines.push_back("LCF/1.0 mode=files");

    int total = response.value("total_matches", 0);
    int unique = response.value("unique_files", 0);
    lines.push_back("total=" + std::to_string(total) +
                    " files=" + std::to_string(unique));

    if (response.contains("files") && response["files"].is_array()) {
        for (const auto& f : response["files"]) {
            if (f.is_string()) {
                lines.push_back(f.get<std::string>());
            }
        }
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

std::string CompactFormatter::format_count_only_response(
    const nlohmann::json& response) const {
    int total = response.value("total_matches", 0);
    int unique = response.value("unique_files", 0);
    return "LCF/1.0 mode=count\ntotal=" + std::to_string(total) +
           " files=" + std::to_string(unique);
}

std::string CompactFormatter::format_context_response(
    const nlohmann::json& response) const {
    std::vector<std::string> lines;
    lines.push_back("LCF/1.0");

    int count = response.value("count", 0);
    lines.push_back("c=" + std::to_string(count));

    if (response.contains("contexts") && response["contexts"].is_array()) {
        for (const auto& ctx : response["contexts"]) {
            lines.push_back(format_object_context(ctx));
        }
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines[i];
    }
    return out;
}

// -- Private helpers ----------------------------------------------------------

std::string CompactFormatter::format_search_result(
    const nlohmann::json& result) const {
    auto file = result.value("file", "");
    int line = result.value("line", 0);
    int col = result.value("column", 0);
    auto oid = result.value("object_id", "");
    double score = result.value("score", 0.0);

    std::ostringstream data;
    data << file << ':' << line << ':' << col
         << " o=" << oid
         << " s=" << static_cast<int>(score);

    if (result.contains("symbol_type") && result["symbol_type"].is_string()) {
        data << " t=" << result["symbol_type"].get<std::string>();
    }
    if (result.contains("symbol_name") && result["symbol_name"].is_string()) {
        data << " n=" << result["symbol_name"].get<std::string>();
    }
    if (result.value("is_exported", false)) {
        data << " e=1";
    }
    if (result.contains("file_match_count") &&
        result["file_match_count"].get<int>() > 0) {
        data << " m=" << result["file_match_count"].get<int>();
    }

    auto match = result.value("match", "");
    if (match.size() > 100) {
        match = match.substr(0, 97) + "...";
    }
    data << ' ' << match;

    std::string line_out = data.str();

    if (include_context && result.contains("context_lines") &&
        result["context_lines"].is_array() &&
        result["context_lines"].size() <= 2) {
        for (const auto& ctx : result["context_lines"]) {
            if (ctx.is_string()) {
                line_out += "\n> " + ctx.get<std::string>();
            }
        }
    }

    if (include_metadata) {
        auto meta = format_metadata(result);
        if (!meta.empty()) {
            line_out += "\n" + meta;
        }
    }

    return line_out;
}

std::string CompactFormatter::format_object_context(
    const nlohmann::json& ctx) const {
    auto path = ctx.value("file_path", "");
    int line = ctx.value("line", 0);
    auto oid = ctx.value("object_id", "");
    auto stype = ctx.value("symbol_type", "");

    std::ostringstream header;
    header << path << ':' << line;

    std::ostringstream data;
    data << "o=" << oid << " t=" << stype;

    if (ctx.contains("symbol_name") && ctx["symbol_name"].is_string()) {
        data << " n=" << ctx["symbol_name"].get<std::string>();
    }
    if (ctx.value("is_exported", false)) {
        data << " e=1";
    }
    if (ctx.contains("signature") && ctx["signature"].is_string()) {
        data << " s=" << ctx["signature"].get<std::string>();
    }

    auto definition = ctx.value("definition", "");
    auto symbol_name = ctx.value("symbol_name", "");
    if (!definition.empty() &&
        (definition.size() < 40 ||
         definition.find(symbol_name) != std::string::npos)) {
        data << " d=" << definition;
    }

    std::string result = header.str() + "\n" + data.str();

    if (include_context && ctx.contains("context") &&
        ctx["context"].is_array() && ctx["context"].size() <= 2) {
        for (const auto& c : ctx["context"]) {
            if (c.is_string()) {
                result += "\n> " + c.get<std::string>();
            }
        }
    }

    return result;
}

std::string CompactFormatter::format_metadata(
    const nlohmann::json& result) const {
    std::vector<std::string> parts;

    if (include_breadcrumbs && result.contains("breadcrumbs") &&
        result["breadcrumbs"].is_array()) {
        std::string bc;
        for (const auto& b : result["breadcrumbs"]) {
            if (b.contains("name") && b["name"].is_string()) {
                if (!bc.empty()) bc += '.';
                bc += b["name"].get<std::string>();
            }
        }
        if (!bc.empty()) {
            parts.push_back("bc=" + bc);
        }
    }

    if (result.contains("safety") && result["safety"].is_object()) {
        auto& safety = result["safety"];
        if (safety.contains("edit_safety")) {
            parts.push_back(
                "safety=" + safety["edit_safety"].get<std::string>());
        }
        if (safety.contains("complexity_score") &&
            safety["complexity_score"].get<double>() > 0) {
            std::ostringstream cs;
            cs.precision(2);
            cs << std::fixed << safety["complexity_score"].get<double>();
            parts.push_back("complexity=" + cs.str());
        }
    }

    if (result.contains("references") && result["references"].is_object()) {
        auto& refs = result["references"];
        int incoming = refs.value("incoming_count", 0);
        int outgoing = refs.value("outgoing_count", 0);
        parts.push_back("refs=" + std::to_string(incoming) + "," +
                        std::to_string(outgoing));
    }

    if (result.contains("dependencies") && result["dependencies"].is_array()) {
        parts.push_back(
            "deps=" + std::to_string(result["dependencies"].size()));
    }

    if (parts.empty()) return "";

    std::string out = "@";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ' ';
        out += parts[i];
    }
    return out;
}

}  // namespace mcp
}  // namespace lci
