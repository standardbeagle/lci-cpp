#include <lci/git/analyzer.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/parser/unified_extractor.h>
#include <lci/core/text.h>
#include <tree_sitter/api.h>

namespace lci {
namespace git {

namespace {

/// File extensions supported for parsing.
bool has_supported_extension(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    auto ext = path.substr(dot);

    static const absl::flat_hash_set<std::string_view> supported = {
        ".go",   ".js",  ".jsx",    ".ts",   ".tsx",  ".py",
        ".rs",   ".java", ".c",     ".cpp",  ".cc",   ".h",
        ".hpp",  ".cs",  ".php",    ".rb",   ".swift", ".kt",
        ".scala", ".zig", ".vue",   ".svelte",
    };
    // Case-insensitive check by lowering (only ASCII extensions).
    std::string lower(ext);
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return supported.contains(lower);
}

int severity_rank(FindingSeverity s) {
    switch (s) {
        case FindingSeverity::Critical: return 3;
        case FindingSeverity::Warning: return 2;
        case FindingSeverity::Info: return 1;
    }
    return 0;
}

/// Common abbreviation -> expansion mappings.
struct AbbrevEntry {
    std::string_view abbrev;
    std::string_view expansions[3];
    int count;
};

const AbbrevEntry kAbbreviations[] = {
    {"usr", {"user", "", ""}, 1},
    {"msg", {"message", "", ""}, 1},
    {"req", {"request", "", ""}, 1},
    {"res", {"response", "result", ""}, 2},
    {"resp", {"response", "", ""}, 1},
    {"btn", {"button", "", ""}, 1},
    {"img", {"image", "", ""}, 1},
    {"err", {"error", "", ""}, 1},
    {"ctx", {"context", "", ""}, 1},
    {"cfg", {"config", "configuration", ""}, 2},
    {"db", {"database", "", ""}, 1},
    {"str", {"string", "", ""}, 1},
    {"num", {"number", "", ""}, 1},
    {"idx", {"index", "", ""}, 1},
    {"len", {"length", "", ""}, 1},
    {"val", {"value", "", ""}, 1},
    {"ptr", {"pointer", "", ""}, 1},
    {"src", {"source", "", ""}, 1},
    {"dst", {"destination", "dest", ""}, 2},
    {"tmp", {"temp", "temporary", ""}, 2},
    {"auth", {"authentication", "authorization", ""}, 2},
    {"info", {"information", "", ""}, 1},
    {"init", {"initialize", "initialization", ""}, 2},
    {"param", {"parameter", "", ""}, 1},
    {"args", {"arguments", "", ""}, 1},
};

int compute_nesting_depth(std::string_view content) {
    int depth = 0;
    int max_depth = 0;
    for (char ch : content) {
        if (ch == '{') {
            ++depth;
            max_depth = std::max(max_depth, depth);
        } else if (ch == '}' && depth > 0) {
            --depth;
        }
    }
    return max_depth > 0 ? max_depth - 1 : 0;
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

Analyzer::Analyzer(Provider& provider, MasterIndex& index)
    : provider_(provider),
      index_(index),
      fuzzy_matcher_(true, 0.8, "jaro-winkler"),
      name_splitter_() {}

// ============================================================================
// Main analysis entry point
// ============================================================================

bool Analyzer::analyze(const AnalysisParams& params, AnalysisReport& out) {
    auto start = std::chrono::steady_clock::now();

    std::vector<ChangedFile> files;
    if (!provider_.get_changed_files(params, files)) return false;

    if (files.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        empty_report(params, elapsed, out);
        return true;
    }

    std::vector<SymbolInfo> new_symbols;
    if (!parse_changed_files(files, params, new_symbols)) return false;

    std::vector<SymbolInfo> existing_symbols;
    get_existing_symbols(existing_symbols);

    std::vector<DuplicateFinding> duplicates;
    std::vector<NamingFinding> naming_issues;
    std::vector<MetricsFinding> metrics_issues;

    if (params.has_focus("duplicates")) {
        find_duplicates(new_symbols, existing_symbols, params, duplicates);
    }
    if (params.has_focus("naming")) {
        check_naming(new_symbols, existing_symbols, params, naming_issues);
    }
    if (params.has_focus("metrics")) {
        check_metrics(new_symbols, existing_symbols, params, metrics_issues);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    build_report(files, new_symbols, duplicates, naming_issues, metrics_issues,
                 params, elapsed, out);
    return true;
}

// ============================================================================
// Symbol extraction
// ============================================================================

bool Analyzer::parse_changed_files(const std::vector<ChangedFile>& files,
                                   const AnalysisParams& params,
                                   std::vector<SymbolInfo>& out) {
    std::string target_ref = provider_.get_target_ref(params);

    for (const auto& file : files) {
        if (file.status == FileChangeStatus::Deleted) continue;
        if (!is_analysis_supported_file(file.path)) continue;

        std::string content;
        if (!provider_.get_file_content(target_ref, file.path, content)) {
            continue;
        }

        auto ext = std::filesystem::path(file.path).extension().string();
        parser::Language lang{};
        if (!parser::language_from_extension(ext, lang)) continue;

        parser::PooledParser parser_guard(lang);
        if (!parser_guard) continue;

        TSTree* tree = ts_parser_parse_string(
            parser_guard.get(), nullptr, content.data(),
            static_cast<uint32_t>(content.size()));
        if (tree == nullptr) continue;

        parser::UnifiedExtractor extractor;
        extractor.init(content, FileID{1}, ext, file.path);
        extractor.extract(tree);
        auto extracted = extractor.get_results();
        ts_tree_delete(tree);

        for (const auto& sym : extracted.symbols) {
            auto type = std::string(to_string(sym.type));
            if (type != "function" && type != "method") continue;

            SymbolInfo si;
            si.name = sym.name;
            si.type = type;
            si.file_path = file.path;
            si.line = sym.line;
            si.end_line = sym.end_line;
            si.lines_of_code = (sym.end_line >= sym.line)
                                   ? (sym.end_line - sym.line + 1)
                                   : 1;
            si.content = extract_symbol_content(content, sym.line, sym.end_line);
            si.nesting_depth = compute_nesting_depth(si.content);

            for (const auto& [pk, cx] : extracted.complexity) {
                if (pk.line == sym.line && pk.column == sym.column) {
                    si.complexity = cx;
                    break;
                }
            }

            out.push_back(std::move(si));
        }
    }
    return true;
}

void Analyzer::get_existing_symbols(std::vector<SymbolInfo>& out) {
    auto file_ids = index_.get_all_file_ids();
    auto rt_snap = index_.ref_tracker().pin();
    for (auto fid : file_ids) {
        std::string path = index_.get_file_path(fid);
        if (path.empty()) continue;
        auto content = index_.file_content_store().get_content(fid);
        auto symbols = rt_snap->get_file_enhanced_symbols(fid);
        for (const auto& sym : symbols) {
            if (sym == nullptr) continue;

            auto type = std::string(to_string(sym->symbol.type));
            if (type != "function" && type != "method") continue;

            SymbolInfo si;
            si.name = sym->symbol.name;
            si.type = type;
            si.file_path = path;
            si.line = sym->symbol.line;
            si.end_line = sym->symbol.end_line;
            si.complexity = sym->complexity;
            si.lines_of_code = (sym->symbol.end_line >= sym->symbol.line)
                                   ? (sym->symbol.end_line - sym->symbol.line + 1)
                                   : 1;
            si.content = extract_symbol_content(content, sym->symbol.line,
                                                sym->symbol.end_line);
            si.nesting_depth = compute_nesting_depth(si.content);
            out.push_back(std::move(si));
        }
    }
}

bool is_analysis_supported_file(std::string_view path) {
    return has_supported_extension(path);
}

std::string extract_symbol_content(std::string_view content,
                                   int start_line, int end_line) {
    if (start_line <= 0 || content.empty()) return {};

    int s = start_line - 1;  // 0-based
    int e = end_line - 1;
    if (e < s) e = s;

    int line = 0;
    size_t line_start = 0;
    size_t start_offset = 0;
    size_t end_offset = content.size();
    bool found_start = false;

    for (size_t i = 0; i <= content.size(); ++i) {
        bool is_end = (i == content.size()) || (content[i] == '\n');
        if (is_end) {
            if (line == s) {
                start_offset = line_start;
                found_start = true;
            }
            if (line == e) {
                end_offset = i;
                break;
            }
            ++line;
            if (i < content.size()) line_start = i + 1;
        }
    }

    if (!found_start) return {};
    return std::string(content.substr(start_offset, end_offset - start_offset));
}

// ============================================================================
// Duplicate detection
// ============================================================================

void Analyzer::find_duplicates(const std::vector<SymbolInfo>& new_symbols,
                               const std::vector<SymbolInfo>& existing_symbols,
                               const AnalysisParams& params,
                               std::vector<DuplicateFinding>& out) {
    double threshold = params.similarity_threshold;
    if (threshold <= 0.0) threshold = 0.8;

    // Build hash map of existing content for exact match.
    absl::flat_hash_map<std::string, std::vector<const SymbolInfo*>> existing_hashes;
    for (const auto& sym : existing_symbols) {
        if (sym.content.empty()) continue;
        auto hash = normalize_code_content(sym.content);
        existing_hashes[hash].push_back(&sym);
    }

    for (const auto& ns : new_symbols) {
        if (ns.content.empty()) continue;
        if (ns.type != "function" && ns.type != "method") continue;

        auto new_hash = normalize_code_content(ns.content);

        // Exact duplicates.
        if (auto it = existing_hashes.find(new_hash); it != existing_hashes.end()) {
            for (const auto* es : it->second) {
                if (es->file_path == ns.file_path && es->line == ns.line) continue;

                DuplicateFinding f;
                f.severity = determine_duplicate_severity(1.0, ns.end_line - ns.line);
                f.description = "Exact duplicate of " + es->name;
                f.new_code = {ns.file_path, ns.line, ns.end_line, ns.name, {}};
                f.existing_code = {es->file_path, es->line, es->end_line, es->name, {}};
                f.similarity = 1.0;
                f.type = "exact";
                f.suggestion = "Extract common code into a shared function";
                out.push_back(std::move(f));
            }
        }

        // Structural duplicates.
        for (const auto& es : existing_symbols) {
            if (es.content.empty()) continue;
            if (es.file_path == ns.file_path && es.line == ns.line) continue;
            if (normalize_code_content(es.content) == new_hash) continue;

            double sim = code_structural_similarity(ns.content, es.content);
            if (sim >= threshold) {
                DuplicateFinding f;
                f.severity = determine_duplicate_severity(sim, ns.end_line - ns.line);
                f.description = "Structurally similar to " + es.name;
                f.new_code = {ns.file_path, ns.line, ns.end_line, ns.name, {}};
                f.existing_code = {es.file_path, es.line, es.end_line, es.name, {}};
                f.similarity = sim;
                f.type = "structural";
                f.suggestion = "Consider parameterizing the common structure";
                out.push_back(std::move(f));
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.similarity > b.similarity;
    });

    int max_findings = params.max_findings > 0 ? params.max_findings : 20;
    if (static_cast<int>(out.size()) > max_findings) {
        out.resize(static_cast<size_t>(max_findings));
    }
}

std::string normalize_code_content(std::string_view content) {
    std::string result;
    result.reserve(content.size());

    size_t i = 0;
    while (i < content.size()) {
        // Find line boundaries.
        auto nl = content.find('\n', i);
        auto line = (nl == std::string_view::npos)
                        ? content.substr(i)
                        : content.substr(i, nl - i);

        // Trim leading/trailing whitespace.
        auto ls = line.find_first_not_of(" \t\r");
        if (ls == std::string_view::npos) {
            i = (nl == std::string_view::npos) ? content.size() : nl + 1;
            continue;
        }
        auto trimmed = line.substr(ls);
        auto re = trimmed.find_last_not_of(" \t\r");
        if (re != std::string_view::npos) trimmed = trimmed.substr(0, re + 1);

        // Skip comment-only and blank lines.
        if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) {
            i = (nl == std::string_view::npos) ? content.size() : nl + 1;
            continue;
        }

        if (!result.empty()) result += '\n';
        result.append(trimmed);
        i = (nl == std::string_view::npos) ? content.size() : nl + 1;
    }
    return result;
}

namespace {

bool is_code_delimiter(char ch) {
    switch (ch) {
        case '(': case ')': case '{': case '}': case '[': case ']':
        case ';': case ',': case '.': case '<': case '>': case '+':
        case '-': case '*': case '/': case '=': case '!': case '&':
        case '|': case '^': case '~': case '?': case ':':
        case ' ': case '\t': case '\n': case '\r':
            return true;
        default:
            return false;
    }
}

void tokenize_code(std::string_view content,
                   std::vector<std::string>& out) {
    std::string current;
    for (char ch : content) {
        if (is_code_delimiter(ch)) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                out.push_back(std::string(1, ch));
            }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
}

}  // namespace

double code_structural_similarity(std::string_view a, std::string_view b) {
    std::vector<std::string> t1, t2;
    tokenize_code(a, t1);
    tokenize_code(b, t2);
    if (t1.empty() || t2.empty()) return 0.0;

    absl::flat_hash_set<std::string_view> s1, s2;
    for (const auto& t : t1) s1.insert(t);
    for (const auto& t : t2) s2.insert(t);

    int intersection = 0;
    for (const auto& t : s1) {
        if (s2.contains(t)) ++intersection;
    }

    int union_size = static_cast<int>(s1.size()) +
                     static_cast<int>(s2.size()) - intersection;
    if (union_size == 0) return 0.0;
    return static_cast<double>(intersection) / static_cast<double>(union_size);
}

// ============================================================================
// Naming consistency
// ============================================================================

void Analyzer::check_naming(const std::vector<SymbolInfo>& new_symbols,
                            const std::vector<SymbolInfo>& existing_symbols,
                            const AnalysisParams& params,
                            std::vector<NamingFinding>& out) {
    double threshold = params.similarity_threshold;
    if (threshold <= 0.0) threshold = 0.8;

    // Group existing by type.
    absl::flat_hash_map<std::string, std::vector<const SymbolInfo*>> by_type;
    for (const auto& sym : existing_symbols) {
        by_type[sym.type].push_back(&sym);
    }

    for (const auto& ns : new_symbols) {
        NamingFinding finding;
        if (check_case_style(ns, finding)) {
            out.push_back(std::move(finding));
        }

        // Build a vector of SymbolInfo for the same type.
        std::vector<SymbolInfo> same_type;
        if (auto it = by_type.find(ns.type); it != by_type.end()) {
            same_type.reserve(it->second.size());
            for (const auto* p : it->second) same_type.push_back(*p);
        }

        NamingFinding similar_finding;
        if (find_similar_names(ns, same_type, threshold, similar_finding)) {
            out.push_back(std::move(similar_finding));
        }

        NamingFinding abbrev_finding;
        if (check_abbreviations(ns, existing_symbols, abbrev_finding)) {
            out.push_back(std::move(abbrev_finding));
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return severity_rank(a.severity) > severity_rank(b.severity);
    });

    int max_findings = params.max_findings > 0 ? params.max_findings : 20;
    if (static_cast<int>(out.size()) > max_findings) {
        out.resize(static_cast<size_t>(max_findings));
    }
}

bool Analyzer::check_case_style(const SymbolInfo& sym, NamingFinding& out) {
    Language lang = get_language_from_path(sym.file_path);
    if (lang == Language::Unknown) return false;

    SymbolKind kind = symbol_type_to_kind(sym.type);
    if (kind == SymbolKind::UnknownKind) return false;

    CaseStyle actual = detect_case_style(sym.name);
    if (actual == CaseStyle::Unknown) return false;

    if (is_valid_case_style(lang, kind, actual)) return false;

    auto expected = get_expected_styles(lang, kind);
    if (expected.empty()) return false;

    std::string expected_str;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (i > 0) {
            expected_str += (i == expected.size() - 1) ? " or " : ", ";
        }
        expected_str += std::string(to_string(expected[i]));
    }

    out.severity = FindingSeverity::Warning;
    out.new_symbol = sym;
    out.issue_type = NamingIssueType::CaseMismatch;
    out.issue = "Uses " + std::string(to_string(actual)) + " but convention is " + expected_str;
    out.suggestion = "Consider renaming to use " + expected_str + " style";
    return true;
}

bool Analyzer::find_similar_names(const SymbolInfo& sym,
                                  const std::vector<SymbolInfo>& existing,
                                  double threshold,
                                  NamingFinding& out) {
    std::vector<SymbolInfo> similar;
    auto new_lower = text::ascii_lower(sym.name);

    for (const auto& es : existing) {
        if (es.name == sym.name) continue;
        auto exist_lower = text::ascii_lower(es.name);
        double sim = fuzzy_matcher_.similarity(new_lower, exist_lower);
        if (sim >= threshold) {
            similar.push_back(es);
        }
    }

    if (similar.empty()) return false;
    if (similar.size() > 3) similar.resize(3);

    std::string names;
    for (size_t i = 0; i < similar.size(); ++i) {
        if (i > 0) names += ", ";
        names += similar[i].name;
    }

    out.severity = determine_naming_severity(NamingIssueType::SimilarExists, threshold);
    out.new_symbol = sym;
    out.similar_names = std::move(similar);
    out.issue_type = NamingIssueType::SimilarExists;
    out.issue = "Similar names already exist: " + names;
    out.suggestion = "Consider using existing name '" + out.similar_names[0].name + "'";
    return true;
}

bool Analyzer::check_abbreviations(const SymbolInfo& sym,
                                   const std::vector<SymbolInfo>& existing,
                                   NamingFinding& out) {
    auto words = name_splitter_.split(sym.name);
    if (words.empty()) return false;

    for (const auto& word : words) {
        auto word_lower = text::ascii_lower(word);

        // Check if word is a known abbreviation.
        for (const auto& entry : kAbbreviations) {
            if (word_lower == entry.abbrev) {
                for (const auto& es : existing) {
                    auto es_words = name_splitter_.split(es.name);
                    for (const auto& ew : es_words) {
                        auto ew_lower = text::ascii_lower(ew);
                        for (int j = 0; j < entry.count; ++j) {
                            if (ew_lower == entry.expansions[j]) {
                                out.severity = FindingSeverity::Info;
                                out.new_symbol = sym;
                                out.similar_names = {es};
                                out.issue_type = NamingIssueType::Abbreviation;
                                out.issue = "Uses abbreviation '" + word +
                                            "' but codebase uses '" + ew + "'";
                                out.suggestion = "Consider using '" + ew +
                                                 "' instead of '" + word + "'";
                                return true;
                            }
                        }
                    }
                }
            }

            // Reverse check: new uses full form, existing uses abbreviation.
            for (int j = 0; j < entry.count; ++j) {
                if (word_lower == entry.expansions[j]) {
                    for (const auto& es : existing) {
                        auto es_words = name_splitter_.split(es.name);
                        for (const auto& ew : es_words) {
                            if (text::ascii_lower(ew) == entry.abbrev) {
                                out.severity = FindingSeverity::Info;
                                out.new_symbol = sym;
                                out.similar_names = {es};
                                out.issue_type = NamingIssueType::Abbreviation;
                                out.issue = "Uses full form '" + word +
                                            "' but codebase uses abbreviation '" + ew + "'";
                                out.suggestion = "Consider using '" + ew +
                                                 "' instead of '" + word + "'";
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// ============================================================================
// Metrics analysis
// ============================================================================

void Analyzer::check_metrics(const std::vector<SymbolInfo>& new_symbols,
                             const std::vector<SymbolInfo>& existing_symbols,
                             const AnalysisParams& params,
                             std::vector<MetricsFinding>& out) {
    auto thresholds = MetricsThresholds::defaults();

    absl::flat_hash_map<std::string, const SymbolInfo*> existing_by_key;
    for (const auto& sym : existing_symbols) {
        if (sym.type == "function" || sym.type == "method") {
            existing_by_key[sym.name + ":" + sym.file_path] = &sym;
        }
    }

    for (const auto& sym : new_symbols) {
        if (sym.type != "function" && sym.type != "method") continue;
        if (sym.complexity == 0 && sym.lines_of_code == 0 && sym.nesting_depth == 0) {
            continue;
        }

        SymbolMetrics new_m{sym.complexity, sym.lines_of_code,
                            sym.nesting_depth, sym.is_pure, sym.side_effects};

        if (sym.complexity > thresholds.high_complexity) {
            MetricsFinding f;
            f.severity = determine_metrics_severity(
                MetricsIssueType::HighComplexity, new_m, thresholds);
            f.description = "Function '" + sym.name +
                            "' has high cyclomatic complexity (" +
                            std::to_string(sym.complexity) + ")";
            f.symbol = sym;
            f.issue_type = MetricsIssueType::HighComplexity;
            f.issue = "Cyclomatic complexity of " + std::to_string(sym.complexity) +
                      " exceeds threshold of " + std::to_string(thresholds.high_complexity);
            f.suggestion =
                "Consider breaking this function into smaller, more focused functions";
            out.push_back(std::move(f));
        }

        if (sym.lines_of_code > thresholds.long_function) {
            MetricsFinding f;
            f.severity = determine_metrics_severity(
                MetricsIssueType::LongFunction, new_m, thresholds);
            f.description = "Function '" + sym.name + "' is too long (" +
                            std::to_string(sym.lines_of_code) + " lines)";
            f.symbol = sym;
            f.issue_type = MetricsIssueType::LongFunction;
            f.issue = "Function has " + std::to_string(sym.lines_of_code) +
                      " lines, exceeding threshold of " +
                      std::to_string(thresholds.long_function);
            f.suggestion =
                "Extract parts of this function into smaller helper functions";
            out.push_back(std::move(f));
        }

        if (sym.nesting_depth > thresholds.deep_nesting) {
            MetricsFinding f;
            f.severity = determine_metrics_severity(
                MetricsIssueType::DeepNesting, new_m, thresholds);
            f.description = "Function '" + sym.name + "' has deep nesting (" +
                            std::to_string(sym.nesting_depth) + " levels)";
            f.symbol = sym;
            f.issue_type = MetricsIssueType::DeepNesting;
            f.issue = "Nesting depth of " + std::to_string(sym.nesting_depth) +
                      " exceeds threshold of " + std::to_string(thresholds.deep_nesting);
            f.suggestion =
                "Reduce nesting by using early returns, extracting functions, or simplifying conditions";
            out.push_back(std::move(f));
        }

        // Check against existing version for growth / purity loss.
        auto key = sym.name + ":" + sym.file_path;
        if (auto it = existing_by_key.find(key); it != existing_by_key.end()) {
            const auto& es = *it->second;
            if (es.complexity > 0 && sym.complexity > es.complexity) {
                double growth = static_cast<double>(sym.complexity - es.complexity) /
                                static_cast<double>(es.complexity) * 100.0;
                if (static_cast<int>(growth) >= thresholds.complexity_growth_threshold) {
                    MetricsFinding f;
                    f.severity = FindingSeverity::Warning;
                    f.description = "Function '" + sym.name + "' complexity grew";
                    f.symbol = sym;
                    f.issue_type = MetricsIssueType::ComplexityGrew;
                    f.issue = "Complexity grew from " + std::to_string(es.complexity) +
                              " to " + std::to_string(sym.complexity);
                    f.suggestion = "Consider refactoring to maintain or reduce complexity";
                    out.push_back(std::move(f));
                }
            }
            if (es.is_pure && !sym.is_pure) {
                MetricsFinding f;
                f.severity = FindingSeverity::Warning;
                f.description = "Function '" + sym.name + "' lost purity";
                f.symbol = sym;
                f.issue_type = MetricsIssueType::PurityLost;
                f.issue = "Previously pure function now has side effects";
                f.suggestion = "Keep pure functions pure or extract impure operations";
                out.push_back(std::move(f));
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return severity_rank(a.severity) > severity_rank(b.severity);
    });

    int max_findings = params.max_findings > 0 ? params.max_findings : 20;
    if (static_cast<int>(out.size()) > max_findings) {
        out.resize(static_cast<size_t>(max_findings));
    }
}

// ============================================================================
// Report building
// ============================================================================

void Analyzer::build_report(const std::vector<ChangedFile>& files,
                            const std::vector<SymbolInfo>& symbols,
                            std::vector<DuplicateFinding>& duplicates,
                            std::vector<NamingFinding>& naming_issues,
                            std::vector<MetricsFinding>& metrics_issues,
                            const AnalysisParams& params,
                            int64_t elapsed_ms,
                            AnalysisReport& out) {
    int symbols_added = 0;
    for (const auto& file : files) {
        if (file.status == FileChangeStatus::Added) {
            for (const auto& sym : symbols) {
                if (sym.file_path == file.path) ++symbols_added;
            }
        }
    }
    int symbols_modified = static_cast<int>(symbols.size()) - symbols_added;

    double risk = calculate_risk_score(duplicates, naming_issues, metrics_issues);
    std::string top_rec = generate_top_recommendation(duplicates, naming_issues, metrics_issues);

    std::string base_ref;
    provider_.get_base_ref(params, base_ref);
    std::string target_ref = provider_.get_target_ref(params);

    out.summary.files_changed = static_cast<int>(files.size());
    out.summary.symbols_added = symbols_added;
    out.summary.symbols_modified = symbols_modified;
    out.summary.duplicates_found = static_cast<int>(duplicates.size());
    out.summary.naming_issues_found = static_cast<int>(naming_issues.size());
    out.summary.metrics_issues_found = static_cast<int>(metrics_issues.size());
    out.summary.risk_score = risk;
    out.summary.top_recommendation = std::move(top_rec);

    out.duplicates = std::move(duplicates);
    out.naming_issues = std::move(naming_issues);
    out.metrics_issues = std::move(metrics_issues);

    out.metadata.base_ref = std::move(base_ref);
    out.metadata.target_ref = std::move(target_ref);
    out.metadata.scope = params.scope;
    out.metadata.analyzed_at = std::chrono::system_clock::now();
    out.metadata.analysis_time_ms = elapsed_ms;
}

void Analyzer::empty_report(const AnalysisParams& params, int64_t elapsed_ms,
                            AnalysisReport& out) {
    std::string base_ref;
    provider_.get_base_ref(params, base_ref);
    std::string target_ref = provider_.get_target_ref(params);

    out = AnalysisReport{};
    out.summary.top_recommendation = "No changes to analyze";
    out.metadata.base_ref = std::move(base_ref);
    out.metadata.target_ref = std::move(target_ref);
    out.metadata.scope = params.scope;
    out.metadata.analyzed_at = std::chrono::system_clock::now();
    out.metadata.analysis_time_ms = elapsed_ms;
}

}  // namespace git
}  // namespace lci
