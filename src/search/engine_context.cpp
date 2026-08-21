#include <lci/search/search_engine.h>

#include <algorithm>
#include <string_view>

namespace lci {

// -- ContextExtractor ---------------------------------------------------------

ContextExtractor::ContextExtractor(const FileContentStore& store,
                                   int default_context_lines)
    : store_(store),
      default_context_lines_(default_context_lines) {}

std::vector<std::string_view> ContextExtractor::split_lines(
    std::string_view content) const {

    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < content.size()) {
        lines.push_back(content.substr(start));
    }
    return lines;
}

int ContextExtractor::find_function_start(
    const std::vector<std::string_view>& lines, int block_start) const {

    int start = block_start;
    for (int i = block_start - 1; i >= 0; --i) {
        auto line = lines[static_cast<size_t>(i)];

        // Trim leading whitespace.
        size_t first_non_space = 0;
        while (first_non_space < line.size() &&
               (line[first_non_space] == ' ' || line[first_non_space] == '\t')) {
            ++first_non_space;
        }
        auto trimmed = line.substr(first_non_space);

        if (trimmed.empty()) continue;

        if (trimmed.starts_with("//") || trimmed.starts_with("/*") ||
            trimmed.starts_with("*") || trimmed.starts_with("@") ||
            trimmed.starts_with("#")) {
            start = i;
            continue;
        }
        break;
    }
    return start;
}

SearchContext ContextExtractor::extract(
    FileID file_id,
    const std::vector<BlockBoundary>& blocks,
    int match_line,
    int max_context_lines) const {

    if (max_context_lines == 0 && !blocks.empty()) {
        return extract_block_context(file_id, blocks, match_line);
    }

    if (max_context_lines > 0 && !blocks.empty()) {
        return extract_function_context(file_id, blocks, match_line,
                                         max_context_lines);
    }

    int ctx_lines = (max_context_lines > 0) ? max_context_lines
                                             : default_context_lines_;
    return extract_line_context(file_id, match_line, ctx_lines);
}

SearchContext ContextExtractor::extract_line_context(
    FileID file_id, int match_line, int num_lines) const {

    SearchContext ctx;
    auto content = store_.get_content(file_id);
    if (content.empty()) return ctx;

    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    // num_lines is the TOTAL size of the window, match line included. The
    // window used to be built as half lines either side of the match plus the
    // match itself, which is right for an odd num_lines but hands back
    // num_lines + 1 lines for an even one -- including the default of 50 and
    // the doubled 100 that extract_block_context falls back to. Split the
    // remainder explicitly and give the extra line to the trailing side, which
    // leaves every odd num_lines emitting exactly the same window as before.
    if (num_lines < 1) num_lines = 1;
    int before = (num_lines - 1) / 2;
    int after = num_lines - 1 - before;
    int start = std::max(0, match_line - 1 - before);
    int end = std::min(total, match_line + after);

    ctx.start_line = start + 1;
    ctx.end_line = end;
    ctx.is_complete = true;

    for (int i = start; i < end; ++i) {
        ctx.lines.emplace_back(lines[static_cast<size_t>(i)]);
    }
    return ctx;
}

SearchContext ContextExtractor::extract_block_context(
    FileID file_id,
    const std::vector<BlockBoundary>& blocks,
    int match_line) const {

    auto content = store_.get_content(file_id);
    if (content.empty()) {
        return extract_line_context(file_id, match_line,
                                     default_context_lines_ * 2);
    }

    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    // Find the smallest block containing the match.
    const BlockBoundary* best = nullptr;
    for (const auto& block : blocks) {
        if (block.start + 1 <= match_line && block.end + 1 >= match_line) {
            if (!best || (block.end - block.start) < (best->end - best->start)) {
                best = &block;
            }
        }
    }

    if (!best) {
        return extract_line_context(file_id, match_line,
                                     default_context_lines_ * 2);
    }

    // Reject unreasonably large blocks (likely parser error).
    int block_length = best->end - best->start + 1;
    if (block_length > 500) {
        return extract_line_context(file_id, match_line,
                                     default_context_lines_ * 2);
    }

    int start = best->start;
    int end = std::min(best->end + 1, total);

    SearchContext ctx;
    ctx.start_line = start + 1;
    ctx.end_line = end;
    ctx.block_type = std::string(to_string(best->type));
    ctx.block_name = best->name;
    ctx.is_complete = true;

    for (int i = start; i < end; ++i) {
        ctx.lines.emplace_back(lines[static_cast<size_t>(i)]);
    }
    return ctx;
}

SearchContext ContextExtractor::extract_function_context(
    FileID file_id,
    const std::vector<BlockBoundary>& blocks,
    int match_line,
    int /*max_context_lines*/) const {

    auto content = store_.get_content(file_id);
    if (content.empty()) {
        return extract_line_context(file_id, match_line, 5);
    }

    auto lines = split_lines(content);
    int total = static_cast<int>(lines.size());

    // Find the smallest containing function/method.
    const BlockBoundary* func = nullptr;
    for (const auto& block : blocks) {
        if ((block.type == BlockType::Function ||
             block.type == BlockType::Method) &&
            block.start + 1 <= match_line && block.end + 1 >= match_line) {
            if (!func || (block.end - block.start) < (func->end - func->start)) {
                func = &block;
            }
        }
    }

    SearchContext ctx;

    if (func) {
        int start = find_function_start(lines, func->start);
        int end = std::min(func->end + 1, total);
        int func_length = end - start;

        ctx.block_type = std::string(to_string(func->type));
        ctx.block_name = func->name;

        if (func_length > 500) {
            // Parser error fallback.
            int padding = 5;
            start = std::max(0, match_line - 1 - padding);
            end = std::min(total, match_line + padding);
            ctx.is_complete = false;
        } else if (func_length > 100) {
            // Long function: center 100 lines on match.
            int match_offset = match_line - 1 - start;
            int window_start = std::max(0, match_offset - 50);
            int window_end = std::min(func_length, window_start + 100);
            if (window_end == func_length) {
                window_start = std::max(0, func_length - 100);
            }
            start = start + window_start;
            // Unclamped on purpose, and safe: `end` came in as
            // min(func->end + 1, total) and func_length = end - start, so
            // start + window_start + 100 never exceeds the function's own
            // end. When window_start + 100 < func_length the window stops
            // short of it; otherwise window_start was just reset to
            // func_length - 100, which lands exactly on it.
            end = start + 100;
            ctx.is_complete = false;
        } else {
            ctx.is_complete = true;
        }

        ctx.start_line = start + 1;
        ctx.end_line = end;
        for (int i = start; i < end && i < total; ++i) {
            ctx.lines.emplace_back(lines[static_cast<size_t>(i)]);
        }
    } else {
        // No containing function; fall back to +/-5 lines.
        int padding = 5;
        int start = std::max(0, match_line - 1 - padding);
        int end = std::min(total, match_line + padding);

        ctx.start_line = start + 1;
        ctx.end_line = end;
        ctx.block_type = "context";
        ctx.is_complete = true;

        for (int i = start; i < end; ++i) {
            ctx.lines.emplace_back(lines[static_cast<size_t>(i)]);
        }
    }

    return ctx;
}

}  // namespace lci
