#include <lci/core/line_scanner.h>

#include <algorithm>
#include <cstring>

namespace lci {

LineScanner::LineScanner(std::string_view content) : content_(content) {
    if (content_.empty()) return;

    // Estimate capacity: ~80 chars per line, minimum 2.
    size_t estimated = content_.size() / 80 + 2;
    if (estimated > 1000) estimated = 1000;
    offsets_.reserve(estimated);

    offsets_.push_back(0);
    for (size_t i = 0; i < content_.size(); ++i) {
        if (content_[i] == '\n' && i + 1 < content_.size()) {
            offsets_.push_back(static_cast<uint32_t>(i + 1));
        }
    }
}

int LineScanner::line_count() const {
    return static_cast<int>(offsets_.size());
}

std::string_view LineScanner::line(int index) const {
    if (index < 0 || index >= static_cast<int>(offsets_.size())) return {};

    uint32_t start = offsets_[static_cast<size_t>(index)];
    uint32_t end;

    if (index + 1 < static_cast<int>(offsets_.size())) {
        end = offsets_[static_cast<size_t>(index + 1)];
    } else {
        end = static_cast<uint32_t>(content_.size());
    }

    // Strip trailing newline and \r for CRLF normalization.
    if (end > start && content_[end - 1] == '\n') {
        --end;
    }
    if (end > start && content_[end - 1] == '\r') {
        --end;
    }

    return content_.substr(start, end - start);
}

uint32_t LineScanner::line_offset(int index) const {
    if (index < 0 || index >= static_cast<int>(offsets_.size())) return 0;
    return offsets_[static_cast<size_t>(index)];
}

int LineScanner::line_at_offset(uint32_t byte_offset) const {
    if (offsets_.empty()) return 0;

    // Binary search for the largest offset <= byte_offset.
    auto it = std::upper_bound(offsets_.begin(), offsets_.end(), byte_offset);
    if (it == offsets_.begin()) return 1;
    --it;
    return static_cast<int>(it - offsets_.begin()) + 1;
}

const std::vector<uint32_t>& LineScanner::offsets() const {
    return offsets_;
}

std::string_view LineScanner::content() const {
    return content_;
}

int count_lines(std::string_view content) {
    if (content.empty()) return 0;

    int newlines = 0;
    for (char c : content) {
        if (c == '\n') ++newlines;
    }

    // If content doesn't end with newline, there's one more line.
    if (content.back() != '\n') {
        return newlines + 1;
    }

    // Content ends with newline - newline count equals line count.
    return newlines == 0 ? 0 : newlines;
}

}  // namespace lci
