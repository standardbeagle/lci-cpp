#include <lci/core/semantic_annotator.h>
#include <lci/core/file_content_store.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>
#include <lci/symbol.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <sstream>

#include <re2/re2.h>

namespace lci {

// ---------------------------------------------------------------------------
// SemanticAnnotator
// ---------------------------------------------------------------------------

namespace {
// Helper: build a quiet RE2 (no stderr spam if the pattern is ever bad).
std::unique_ptr<RE2> mk(const char* pat) {
    RE2::Options opts(RE2::Quiet);
    opts.set_log_errors(false);
    return std::make_unique<RE2>(pat, opts);
}
}  // namespace

SemanticAnnotator::Patterns SemanticAnnotator::make_patterns() {
    Patterns p;
    p.labels             = mk(R"(@lci:labels?\[([^\]]+)\])");
    p.category           = mk(R"(@lci:category\[([^\]]+)\])");
    p.tags               = mk(R"(@lci:tags?\[([^\]]+)\])");
    p.deps               = mk(R"(@lci:deps?\[([^\]]+)\])");
    p.provides           = mk(R"(@lci:provides?\[([^\]]+)\])");
    p.metrics            = mk(R"(@lci:metrics?\[([^\]]+)\])");
    p.attr               = mk(R"(@lci:attr(?:ibutes?)?\[([^\]]+)\])");
    p.exclude            = mk(R"(@lci:exclude\[([^\]]+)\])");
    p.loop_weight        = mk(R"(@lci:loop-weight\[([0-9.]+)\])");
    p.loop_bounded       = mk(R"(@lci:loop-bounded\[([0-9]+)\])");
    p.call_frequency     = mk(R"(@lci:call-frequency\[([^\]]+)\])");
    p.propagation_weight = mk(R"(@lci:propagation-weight\[([0-9.]+)\])");
    return p;
}

SemanticAnnotator::SemanticAnnotator() : patterns_(make_patterns()) {}

void SemanticAnnotator::extract_annotations(
    FileID file_id, std::string_view file_path, std::string_view content,
    const std::vector<Symbol>& symbols) {

    // Split content into lines
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, end - start));
        start = end + 1;
    }

    for (const auto& symbol : symbols) {
        auto* annotation = extract_symbol_annotation(file_id, symbol, lines);
        if (annotation) {
            SymbolID sym_id = static_cast<SymbolID>(file_id) << 32 |
                              static_cast<SymbolID>(symbol.line) << 16 |
                              static_cast<SymbolID>(symbol.column);

            annotation->file_id = file_id;
            annotation->symbol_id = sym_id;

            // Build the AnnotatedSymbol once and clone into each per-key
            // index. Move into annotations_ at the end so the file/symbol map
            // takes ownership of the annotation body.
            AnnotatedSymbol entry{
                file_id, sym_id, symbol.name, symbol.line,
                *annotation, std::string(file_path)};

            // Index by labels
            for (const auto& label : annotation->labels) {
                label_index_[label].push_back(entry);
            }
            // Index by category (single-valued, optional).
            if (!annotation->category.empty()) {
                category_index_[annotation->category].push_back(entry);
            }

            annotations_[file_id][sym_id] = std::move(*annotation);
        }
    }
}

const SemanticAnnotation* SemanticAnnotator::get_annotation(
    FileID file_id, SymbolID symbol_id) const {
    auto file_it = annotations_.find(file_id);
    if (file_it == annotations_.end()) return nullptr;
    auto sym_it = file_it->second.find(symbol_id);
    if (sym_it == file_it->second.end()) return nullptr;
    return &sym_it->second;
}

std::vector<const AnnotatedSymbol*> SemanticAnnotator::get_symbols_by_label(
    std::string_view label) const {
    std::vector<const AnnotatedSymbol*> result;
    auto it = label_index_.find(std::string(label));
    if (it != label_index_.end()) {
        result.reserve(it->second.size());
        for (const auto& sym : it->second) {
            result.push_back(&sym);
        }
    }
    return result;
}

std::vector<const AnnotatedSymbol*> SemanticAnnotator::get_symbols_by_category(
    std::string_view category) const {
    std::vector<const AnnotatedSymbol*> result;
    auto it = category_index_.find(std::string(category));
    if (it != category_index_.end()) {
        result.reserve(it->second.size());
        for (const auto& sym : it->second) {
            result.push_back(&sym);
        }
    }
    return result;
}

int SemanticAnnotator::populate_from_index(const MasterIndex& index) {
    // Walk every indexed file. For each: pull file content + enhanced symbols
    // from the content store + reference tracker, lower EnhancedSymbol* into
    // a vector<Symbol> (extract_annotations's expected shape), then call the
    // existing extraction path. This is the per-MCP-session annotator
    // population step — without it the tool sees only externally seeded
    // labels and category queries return empty.
    int processed = 0;
    const auto& content_store = index.file_content_store();
    const auto& ref = index.ref_tracker();

    std::vector<Symbol> file_symbols;  // reused across files (Karpathy: no
                                       // per-file allocator)
    for (FileID fid : index.get_all_file_ids()) {
        auto content = content_store.get_content(fid);
        if (content.empty()) continue;
        auto enhanced = ref.get_file_enhanced_symbols(fid);
        if (enhanced.empty()) continue;

        file_symbols.clear();
        file_symbols.reserve(enhanced.size());
        for (const auto* es : enhanced) {
            if (es) file_symbols.push_back(es->symbol);
        }
        if (file_symbols.empty()) continue;

        std::string path = index.get_file_path(fid);
        extract_annotations(fid, path, content, file_symbols);
        ++processed;
    }
    return processed;
}

bool SemanticAnnotator::is_excluded(FileID file_id, SymbolID symbol_id,
                                    std::string_view analysis_type) const {
    auto* ann = get_annotation(file_id, symbol_id);
    if (!ann || ann->excludes.empty()) return false;

    std::string lower_type(analysis_type);
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& exclude : ann->excludes) {
        std::string lower_exclude = exclude;
        std::transform(lower_exclude.begin(), lower_exclude.end(),
                       lower_exclude.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_exclude == "all" || lower_exclude == lower_type) return true;
    }
    return false;
}

int SemanticAnnotator::total_annotations() const {
    int count = 0;
    for (const auto& [_, file_anns] : annotations_) {
        count += static_cast<int>(file_anns.size());
    }
    return count;
}

int SemanticAnnotator::unique_labels() const {
    return static_cast<int>(label_index_.size());
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

bool SemanticAnnotator::is_annotation_line(std::string_view line) const {
    return line.find("@lci:") != std::string_view::npos;
}

SemanticAnnotation* SemanticAnnotator::extract_symbol_annotation(
    FileID /*file_id*/, const Symbol& symbol,
    const std::vector<std::string_view>& lines) {

    int start_search = std::max(0, symbol.line - 10);
    int end_search = symbol.line - 1;

    std::vector<std::string_view> annotation_lines;
    std::string raw_text;

    for (int i = start_search; i <= end_search &&
                                i < static_cast<int>(lines.size()); ++i) {
        auto line = lines[static_cast<size_t>(i)];
        // Trim leading whitespace for check
        auto trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.remove_prefix(1);
        }
        if (is_annotation_line(trimmed)) {
            annotation_lines.push_back(trimmed);
            raw_text.append(trimmed);
            raw_text.push_back('\n');
        }
    }

    if (annotation_lines.empty()) return nullptr;

    // Use thread-local storage for the annotation being built
    thread_local SemanticAnnotation annotation;
    annotation = SemanticAnnotation{};
    annotation.raw_text = raw_text;
    annotation.start_line = start_search;
    annotation.end_line = end_search;
    annotation.confidence = 1.0;

    for (auto line : annotation_lines) {
        parse_annotation_line(line, annotation);
    }

    // Only return if we found content
    bool has_content = !annotation.labels.empty() ||
                       !annotation.category.empty() ||
                       !annotation.tags.empty() ||
                       !annotation.metrics.empty() ||
                       !annotation.attributes.empty() ||
                       !annotation.excludes.empty() ||
                       annotation.has_memory_hints;

    if (!has_content) return nullptr;
    return &annotation;
}

void SemanticAnnotator::parse_annotation_line(std::string_view line,
                                              SemanticAnnotation& ann) {
    // RE2 operates directly on StringPiece — no std::string copy of `line`.
    // Each capture lands in a stack-local std::string (smallest necessary
    // owning buffer for the captured tag).
    re2::StringPiece sp(line.data(), line.size());
    std::string cap;

    // Labels
    if (RE2::PartialMatch(sp, *patterns_.labels, &cap)) {
        auto labels = parse_comma_separated(cap);
        ann.labels.insert(ann.labels.end(), labels.begin(), labels.end());
    }

    // Category
    if (RE2::PartialMatch(sp, *patterns_.category, &cap)) {
        ann.category = cap;
        // Trim
        while (!ann.category.empty() && ann.category.front() == ' ')
            ann.category.erase(ann.category.begin());
        while (!ann.category.empty() && ann.category.back() == ' ')
            ann.category.pop_back();
    }

    // Tags
    if (RE2::PartialMatch(sp, *patterns_.tags, &cap)) {
        auto tags = parse_key_value_pairs(cap);
        for (auto& [k, v] : tags) ann.tags[k] = v;
    }

    // Provides
    if (RE2::PartialMatch(sp, *patterns_.provides, &cap)) {
        auto provides = parse_comma_separated(cap);
        ann.provides.insert(ann.provides.end(), provides.begin(),
                            provides.end());
    }

    // Metrics
    if (RE2::PartialMatch(sp, *patterns_.metrics, &cap)) {
        auto metrics = parse_key_value_pairs(cap);
        for (auto& [k, v] : metrics) ann.metrics[k] = v;
    }

    // Attributes
    if (RE2::PartialMatch(sp, *patterns_.attr, &cap)) {
        auto attrs = parse_key_value_pairs(cap);
        for (auto& [k, v] : attrs) ann.attributes[k] = v;
    }

    // Excludes
    if (RE2::PartialMatch(sp, *patterns_.exclude, &cap)) {
        auto excludes = parse_comma_separated(cap);
        ann.excludes.insert(ann.excludes.end(), excludes.begin(),
                            excludes.end());
    }

    // Memory hints
    parse_memory_hints(line, ann);
}

void SemanticAnnotator::parse_memory_hints(std::string_view line,
                                           SemanticAnnotation& ann) {
    re2::StringPiece sp(line.data(), line.size());
    std::string cap;

    if (RE2::PartialMatch(sp, *patterns_.loop_weight, &cap)) {
        double weight = 0;
        auto [ptr, ec] = std::from_chars(cap.data(), cap.data() + cap.size(),
                                         weight);
        if (ec == std::errc{}) {
            ann.loop_weight = weight;
            ann.has_memory_hints = true;
        }
    }

    if (RE2::PartialMatch(sp, *patterns_.loop_bounded, &cap)) {
        int bounded = 0;
        auto [ptr, ec] = std::from_chars(cap.data(), cap.data() + cap.size(),
                                         bounded);
        if (ec == std::errc{}) {
            ann.loop_bounded = bounded;
            ann.has_memory_hints = true;
        }
    }

    if (RE2::PartialMatch(sp, *patterns_.call_frequency, &cap)) {
        std::string freq = cap;
        while (!freq.empty() && freq.front() == ' ') freq.erase(freq.begin());
        while (!freq.empty() && freq.back() == ' ') freq.pop_back();
        if (is_valid_call_frequency(freq)) {
            ann.call_frequency = freq;
            ann.has_memory_hints = true;
        }
    }

    if (RE2::PartialMatch(sp, *patterns_.propagation_weight, &cap)) {
        double weight = 0;
        auto [ptr, ec] = std::from_chars(cap.data(), cap.data() + cap.size(),
                                         weight);
        if (ec == std::errc{}) {
            ann.propagation_weight = std::clamp(weight, 0.0, 1.0);
            ann.has_memory_hints = true;
        }
    }
}

std::vector<std::string> SemanticAnnotator::parse_comma_separated(
    std::string_view input) const {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < input.size()) {
        size_t end = input.find(',', start);
        if (end == std::string_view::npos) end = input.size();
        auto part = input.substr(start, end - start);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (!part.empty()) result.emplace_back(part);
        start = end + 1;
    }
    return result;
}

absl::flat_hash_map<std::string, std::string>
SemanticAnnotator::parse_key_value_pairs(std::string_view input) const {
    absl::flat_hash_map<std::string, std::string> result;
    auto parts = parse_comma_separated(input);
    for (const auto& pair : parts) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            auto key = pair.substr(0, eq);
            auto value = pair.substr(eq + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!value.empty() && value.front() == ' ')
                value.erase(value.begin());
            result[key] = value;
        }
    }
    return result;
}

bool SemanticAnnotator::is_valid_call_frequency(std::string_view freq) {
    static const absl::flat_hash_map<std::string, bool> valid = {
        {"hot-path", true},
        {"once-per-file", true},
        {"once-per-request", true},
        {"once-per-session", true},
        {"startup-only", true},
        {"cli-output", true},
        {"test-only", true},
        {"rare", true},
    };
    return valid.contains(std::string(freq));
}

}  // namespace lci
