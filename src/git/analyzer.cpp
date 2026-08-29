#include <lci/git/analyzer.h>

#include <lci/analysis/scope_set.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <lci/parser/parser.h>
#include <lci/parser/parser_pool.h>
#include <lci/parser/unified_extractor.h>
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
        ".pyx",  ".pxd",
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
    : provider_(provider), index_(index) {}

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
    int skipped_unreadable = 0;
    if (!parse_changed_files(files, params, new_symbols, skipped_unreadable))
        return false;

    // Scope to the CHANGE, not the file: parse_changed_files parses whole
    // files, so without this every pre-existing symbol in a touched file
    // was analyzed as "new" — a 33-line commit reported ~100 modified
    // symbols, findings mostly described untouched code, and risk pegged
    // at 1.00. Symbols in Added files stay whole-file (every line is new).
    // A failed hunk fetch keeps the unscoped set (over-report, never drop).
    ScopeSet changed_scope;
    if (provider_.get_changed_scope(params, changed_scope) &&
        !changed_scope.is_all()) {
        absl::flat_hash_set<std::string> added_files;
        for (const auto& f : files) {
            if (f.status == FileChangeStatus::Added) added_files.insert(f.path);
        }
        std::vector<SymbolInfo> scoped;
        scoped.reserve(new_symbols.size());
        for (auto& si : new_symbols) {
            if (added_files.contains(si.file_path) ||
                changed_scope.contains_lines(si.file_path, si.line,
                                             si.end_line)) {
                scoped.push_back(std::move(si));
            }
        }
        new_symbols = std::move(scoped);
    }

    std::vector<SymbolInfo> existing_symbols;
    get_existing_symbols(existing_symbols);

    std::vector<DuplicateFinding> duplicates;
    std::vector<NamingFinding> naming_issues;
    std::vector<MetricsFinding> metrics_issues;

    if (params.has_focus("duplicates")) {
        find_duplicates(new_symbols, existing_symbols, params, duplicates);
    }
    if (params.has_focus("naming")) {
        check_naming(new_symbols, params, naming_issues);
    }
    if (params.has_focus("metrics")) {
        check_metrics(new_symbols, existing_symbols, params, metrics_issues);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    build_report(files, new_symbols, duplicates, naming_issues, metrics_issues,
                 params, elapsed, skipped_unreadable, out);
    return true;
}

// ============================================================================
// Symbol extraction
// ============================================================================

bool Analyzer::parse_changed_files(const std::vector<ChangedFile>& files,
                                   const AnalysisParams& params,
                                   std::vector<SymbolInfo>& out,
                                   int& skipped_out) {
    std::string target_ref = provider_.get_target_ref(params);
    skipped_out = 0;

    for (const auto& file : files) {
        if (file.status == FileChangeStatus::Deleted) continue;
        if (!is_analysis_supported_file(file.path)) continue;

        std::string content;
        if (!provider_.get_file_content(target_ref, file.path, content)) {
            // A supported source file that cannot be read contributes no
            // symbols, so every finding for it silently disappears. Count and
            // announce it rather than letting the report claim coverage it
            // does not have (Karpathy rule 6).
            ++skipped_out;
            std::fprintf(stderr,
                         "warning: git analysis skipped %s: content "
                         "unreadable at ref %s\n",
                         file.path.c_str(), target_ref.c_str());
            continue;
        }

        auto ext = std::filesystem::path(file.path).extension().string();
        parser::Language lang{};
        if (!parser::language_from_extension(ext, lang)) continue;

        parser::PooledParser parser_guard(lang);
        if (!parser_guard) continue;

        parser::UniqueTree tree(ts_parser_parse_string(
            parser_guard.get(), nullptr, content.data(),
            static_cast<uint32_t>(content.size())));
        if (!tree) continue;

        parser::UnifiedExtractor extractor;
        extractor.init(content, FileID{1}, ext, file.path);
        extractor.extract(tree.get());
        auto extracted = extractor.get_results();

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
        // Repo-relative, like the git diff's changed-file paths: the
        // duplicate finder's same-location guard compares the two, and an
        // absolute-vs-relative mismatch made every changed symbol "duplicate"
        // its own indexed copy (self-match noise findings).
        std::string path = normalize_rel(index_.get_file_path(fid),
                                         provider_.repo_root());
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

    // Normalize each existing symbol EXACTLY ONCE. The structural-duplicate
    // loop below re-normalized es.content for every (new x existing) pair —
    // an O(new * existing * bytes) rescan of content this map had already
    // normalized. Keys are views into `existing_norm`, which is reserved up
    // front so no emplace_back can reallocate it.
    std::vector<std::string> existing_norm;
    existing_norm.reserve(existing_symbols.size());
    absl::flat_hash_map<std::string_view, std::vector<const SymbolInfo*>>
        existing_hashes;
    existing_hashes.reserve(existing_symbols.size());
    for (const auto& sym : existing_symbols) {
        existing_norm.push_back(sym.content.empty()
                                    ? std::string()
                                    : normalize_code_content(sym.content));
        if (sym.content.empty()) continue;
        existing_hashes[existing_norm.back()].push_back(&sym);
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
        for (size_t ei = 0; ei < existing_symbols.size(); ++ei) {
            const auto& es = existing_symbols[ei];
            if (es.content.empty()) continue;
            if (es.file_path == ns.file_path && es.line == ns.line) continue;
            if (existing_norm[ei] == new_hash) continue;

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

void naming_findings_from_report(const NamingReport& report,
                                 const std::vector<const SymbolInfo*>& changed,
                                 std::vector<NamingFinding>& out) {
    absl::flat_hash_map<std::string_view, const SymbolInfo*> by_name;
    by_name.reserve(changed.size());
    for (const auto* s : changed) by_name.emplace(s->name, s);

    for (const auto& split : report.synonym_splits) {
        if (split.members.size() < 2) continue;
        for (const auto& m : split.members) {
            auto it = by_name.find(m.name);
            if (it == by_name.end()) continue;
            std::string others;
            const SynonymSplitMember* top = nullptr;
            for (const auto& o : split.members) {
                if (o.name == m.name) continue;
                if (!others.empty()) others += ", ";
                others += o.name;
                if (top == nullptr || o.fan_in > top->fan_in) top = &o;
            }
            if (top == nullptr) continue;
            NamingFinding f;
            f.issue_type = NamingIssueType::SynonymSplit;
            f.severity = determine_naming_severity(f.issue_type);
            f.new_symbol = *it->second;
            f.description = "Synonym split of '" + split.canonical + "'";
            f.issue = "Same concept already spelled differently: " + others;
            f.suggestion = "Use one spelling — highest fan-in is '" +
                           top->name + "'";
            out.push_back(std::move(f));
        }
    }

    for (const auto& an : report.ambiguous_names) {
        auto it = by_name.find(an.name);
        if (it == by_name.end()) continue;
        NamingFinding f;
        f.issue_type = NamingIssueType::AmbiguousName;
        f.severity = determine_naming_severity(f.issue_type);
        f.new_symbol = *it->second;
        f.description = "Ambiguous name '" + an.name + "'";
        f.issue = "Already defined at " + std::to_string(an.definition_count) +
                  " sites — a search on this name identifies nothing";
        f.suggestion = "Add a distinguishing token to the name";
        out.push_back(std::move(f));
    }

    for (const auto& vn : report.information.vague_names) {
        auto it = by_name.find(vn.name);
        if (it == by_name.end()) continue;
        NamingFinding f;
        f.issue_type = NamingIssueType::VagueName;
        f.severity = determine_naming_severity(f.issue_type);
        f.new_symbol = *it->second;
        f.description = "Vague name '" + vn.name + "'";
        f.issue = "Name only narrows the corpus to ~" +
                  std::to_string(static_cast<int>(vn.expected_matches)) +
                  " candidate symbols";
        f.suggestion = "Add a more selective token to the name";
        out.push_back(std::move(f));
    }

    for (const auto& o : report.outliers) {
        auto it = by_name.find(o.name);
        if (it == by_name.end()) continue;
        NamingFinding f;
        f.issue_type = NamingIssueType::VocabularyOutlier;
        f.severity = determine_naming_severity(f.issue_type);
        f.new_symbol = *it->second;
        f.description = "Vocabulary outlier '" + o.name + "'";
        f.issue = "Token '" + o.odd_term + "' (" + o.reason +
                  ") is vocabulary an agent is unlikely to search for";
        f.suggestion = o.suggested.empty()
                           ? std::string("Use standard vocabulary for this "
                                         "token")
                           : "Consider '" + o.suggested[0] + "'";
        out.push_back(std::move(f));
    }
}

void Analyzer::check_naming(const std::vector<SymbolInfo>& new_symbols,
                            const AnalysisParams& params,
                            std::vector<NamingFinding>& out) {
    std::vector<const SymbolInfo*> changed;
    changed.reserve(new_symbols.size());
    for (const auto& ns : new_symbols) {
        // Macro-expansion names (TEST, TEST_F, EXPECT_*) are not naming
        // choices: the parser sees the macro identifier as the function
        // name. Every prior "similar to TEST_F" finding was this noise.
        bool macro_like = !ns.name.empty();
        for (char c : ns.name) {
            if (!(std::isupper(static_cast<unsigned char>(c)) || c == '_' ||
                  std::isdigit(static_cast<unsigned char>(c)))) {
                macro_like = false;
                break;
            }
        }
        if (macro_like) continue;
        changed.push_back(&ns);
    }
    if (changed.empty()) return;

    for (const auto* ns : changed) {
        NamingFinding finding;
        if (check_case_style(*ns, finding)) {
            out.push_back(std::move(finding));
        }
    }

    // Corpus-relative signals come from the report-side NamingAnalyzer
    // (synonym splits, ambiguous names, vague names, vocabulary outliers),
    // computed once over the index and filtered to the changed symbols. The
    // NamingAnalyzer caps its per-signal lists, so a changed name can miss
    // the cut on a corpus with worse offenders — over-report is bounded, not
    // guaranteed exhaustive.
    std::vector<FileSymbolData> files;
    auto rt_snap = index_.ref_tracker().pin();
    for (auto fid : index_.get_all_file_ids()) {
        auto syms = rt_snap->get_file_enhanced_symbols(fid);
        if (syms.empty()) continue;
        FileSymbolData fsd;
        fsd.path = index_.get_file_path(fid);
        fsd.owner = rt_snap;
        fsd.symbols.reserve(syms.size());
        for (const auto& sym : syms) fsd.symbols.push_back(sym.get());
        files.push_back(std::move(fsd));
    }
    auto report = NamingAnalyzer().analyze(files, index_.config().synonyms,
                                           index_.config().project.root);
    naming_findings_from_report(report, changed, out);

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
                            int64_t elapsed_ms, int skipped_unreadable,
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
    out.metadata.files_skipped_unreadable = skipped_unreadable;
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
