#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace lci::server_request {

struct Search {
    std::string pattern;
    int max_results{100};
    int max_context_lines{1};
    bool case_insensitive{};
    bool declaration_only{};
    // Trailing CLI path args (`lci grep pattern <path>...`). Each entry is a
    // root-relative file or directory prefix; results are scoped index-side.
    std::vector<std::string> paths;
};

struct LimitedPattern {
    std::string pattern;
    int max_results{100};
};

template <typename T>
bool optional_field(const nlohmann::json& body, const char* name, T& value,
                    std::string& error) {
    auto it = body.find(name);
    if (it == body.end()) return true;
    if constexpr (std::is_same_v<T, int>) {
        if (!it->is_number_integer()) {
            error = std::string(name) + " must be an integer";
            return false;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            error = std::string(name) + " must be a boolean";
            return false;
        }
    }
    value = it->get<T>();
    return true;
}

inline bool pattern(const nlohmann::json& body, std::string& value,
                    std::string& error) {
    if (!body.is_object()) {
        error = "JSON body must be an object";
        return false;
    }
    auto it = body.find("pattern");
    if (it == body.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        error = "pattern is required and must be a non-empty string";
        return false;
    }
    value = it->get<std::string>();
    return true;
}

inline std::optional<Search> decode_search(const nlohmann::json& body,
                                           std::string& error) {
    Search request;
    if (!pattern(body, request.pattern, error) ||
        !optional_field(body, "max_results", request.max_results, error) ||
        !optional_field(body, "max_context_lines", request.max_context_lines,
                        error) ||
        !optional_field(body, "case_insensitive", request.case_insensitive,
                        error) ||
        !optional_field(body, "declaration_only", request.declaration_only,
                        error)) {
        return std::nullopt;
    }
    // Decode the optional `paths` array (multi-path CLI scoping). Absent =
    // unscoped (empty vector). Must be an array of strings when present.
    if (auto it = body.find("paths"); it != body.end()) {
        if (!it->is_array()) {
            error = "paths must be an array of strings";
            return std::nullopt;
        }
        for (const auto& el : *it) {
            if (!el.is_string()) {
                error = "paths must be an array of strings";
                return std::nullopt;
            }
            request.paths.push_back(el.get<std::string>());
        }
    }
    if (request.max_results <= 0) request.max_results = 100;
    request.max_results = std::min(request.max_results, 1000);
    request.max_context_lines = std::clamp(request.max_context_lines, 0, 100);
    return request;
}

inline std::optional<LimitedPattern> decode_limited_pattern(
    const nlohmann::json& body, std::string& error) {
    LimitedPattern request;
    if (!pattern(body, request.pattern, error) ||
        !optional_field(body, "max_results", request.max_results, error)) {
        return std::nullopt;
    }
    if (request.max_results <= 0) request.max_results = 100;
    request.max_results = std::min(request.max_results, 1000);
    return request;
}

}  // namespace lci::server_request
