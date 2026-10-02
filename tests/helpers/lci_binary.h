#pragma once

// Locates the built `lci` executable relative to the running test binary.
//
// Single-config generators (Ninja, Make) lay out build/<preset>/tests/lci_tests
// next to build/<preset>/src/lci. Multi-config generators (the Visual Studio
// generator on Windows) add a configuration directory to both and an .exe
// suffix: build/<preset>/tests/<Config>/lci_tests.exe beside
// build/<preset>/src/<Config>/lci.exe.

#include <filesystem>

#include <lci/core/portable.h>

namespace lci {
namespace test {

inline std::filesystem::path lci_binary_path() {
    const auto test_dir = portable::executable_path().parent_path();
#ifdef _WIN32
    return test_dir.parent_path().parent_path() / "src" /
           test_dir.filename() / "lci.exe";
#else
    return test_dir.parent_path() / "src" / "lci";
#endif
}

}  // namespace test
}  // namespace lci
