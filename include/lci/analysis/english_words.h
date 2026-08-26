#pragma once

#include <string_view>

namespace lci::analysis {

/// True if `lower_token` is an English word in the embedded SCOWL list
/// (src/analysis/data/, ~95k lowercase words). Exact match only.
bool is_english_word(std::string_view lower_token);

/// True if `lower_token` is real English in the wider sense the vocabulary
/// analyzer needs: an exact SCOWL word, a standard-prefix derivation of one
/// (un+scoped), or a form whose Porter2 stem is one (serializer -> serial).
/// A token that passes can never be a "misspelling" or "obscure-token"
/// outlier — flagging real English as broken vocabulary is the anti-signal
/// that makes agents distrust the whole report.
bool is_english_like_token(std::string_view lower_token);

}  // namespace lci::analysis
