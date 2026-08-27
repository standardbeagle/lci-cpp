#pragma once

// Internals shared between the CLI command translation units after the
// commands.cpp split. Not a public API.

#include <string>

namespace lci {

class Client;

namespace cli {

/// Prints near-miss suggestions after an empty def/inspect lookup.
/// Returns true if any suggestion was printed.
bool print_symbol_suggestions(Client& client, const std::string& symbol);

}  // namespace cli
}  // namespace lci
