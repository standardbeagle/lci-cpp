#include <lci/semantic/stemmer.h>

#include <libstemmer.h>

#include <string>

// libstemmer (Snowball) wrapper. Replaces a 449-LOC hand-port of Porter2.
// Three-way byte-equivalence with Go surgebase/porter2 is enforced by
// tests/data/porter2_fixture/{voc.txt,output.txt} — the canonical Snowball
// English fixture, which is also the surgebase Go acceptance fixture.
//
// Karpathy hot-path discipline: one sb_stemmer per thread (thread_local),
// reused across calls; sb_stemmer_new mallocs ~10KB so it must never be
// paid per token. Output is copied from libstemmer's internal buffer once
// per call — no allocation in libstemmer itself for typical word lengths.

namespace lci {
namespace {

struct StemmerHandle {
    sb_stemmer* sb = nullptr;
    StemmerHandle() : sb(sb_stemmer_new("english", "UTF_8")) {}
    ~StemmerHandle() { if (sb) sb_stemmer_delete(sb); }
};

sb_stemmer* thread_local_sb() {
    thread_local StemmerHandle h;
    return h.sb;
}

// Snowball English handles lowercase ASCII letters plus the ASCII
// apostrophe (Step 0 strips 's, 's, '). Non-ASCII or uppercase falls
// through unchanged to match surgebase behavior on the voc.txt fixture
// (no uppercase or non-ASCII appear there).
bool is_stemmable_ascii(std::string_view w) {
    for (char c : w) {
        if (!((c >= 'a' && c <= 'z') || c == '\'')) return false;
    }
    return true;
}

}  // namespace

std::string porter2_stem(std::string_view input) {
    if (input.empty()) return std::string(input);
    // Snowball English stems lowercase ASCII; non-lower-alpha falls through
    // unchanged, matching surgebase behavior on the voc.txt fixture.
    if (!is_stemmable_ascii(input)) return std::string(input);
    sb_stemmer* sb = thread_local_sb();
    if (!sb) return std::string(input);
    const sb_symbol* out = sb_stemmer_stem(
        sb, reinterpret_cast<const sb_symbol*>(input.data()),
        static_cast<int>(input.size()));
    if (!out) return std::string(input);
    return std::string(reinterpret_cast<const char*>(out),
                       static_cast<size_t>(sb_stemmer_length(sb)));
}

Stemmer::Stemmer(bool enabled, std::string_view algorithm,
                 int min_length, std::unordered_set<std::string> exclusions)
    : enabled_(enabled), algorithm_(algorithm),
      min_length_(min_length), exclusions_(std::move(exclusions)) {}

Stemmer Stemmer::disabled() { return Stemmer(false, "porter2", 3, {}); }

std::string Stemmer::stem(std::string_view word) const {
    if (!enabled_) return std::string(word);
    if (static_cast<int>(word.size()) < min_length_) return std::string(word);
    if (is_excluded(word)) return std::string(word);
    return porter2_stem(word);
}

std::vector<std::string> Stemmer::stem_all(
    const std::vector<std::string>& words) const {
    std::vector<std::string> out;
    out.reserve(words.size());
    for (const auto& w : words) out.push_back(stem(w));
    return out;
}

std::unordered_map<std::string, std::vector<std::string>>
Stemmer::stem_and_group(const std::vector<std::string>& words) const {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    groups.reserve(words.size());
    for (const auto& w : words) groups[stem(w)].push_back(w);
    return groups;
}

bool Stemmer::is_excluded(std::string_view word) const {
    return exclusions_.count(std::string(word)) > 0;
}

}  // namespace lci
