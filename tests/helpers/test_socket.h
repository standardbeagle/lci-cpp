#pragma once

// Cross-platform test server addressing.
//
// On POSIX the IndexServer listens on a Unix-domain socket (a ".sock" path).
// Windows has no AF_UNIX, so production falls back to TCP "localhost:<port>"
// (see get_socket_path() / server.cpp). A test fixture must therefore hand the
// server — and any raw httplib client — an address of the platform-correct
// shape. A Windows temp ".sock" path is "C:\...\x.sock", and the server's port
// parser (stoi after the last ':') hits the drive-letter colon and throws in
// SetUp(), failing every test in the fixture. These helpers keep that
// divergence in one place.

#include <atomic>
#include <chrono>
#include <string>

#ifndef _WIN32
#include <filesystem>
#include <sys/un.h>
#endif

#include <httplib.h>

namespace lci {
namespace test {

// Process-unique address suitable for IndexServer::set_socket_path().
inline std::string next_test_server_address() {
    static std::atomic<int> counter{0};
    int n = counter.fetch_add(1);
#ifdef _WIN32
    // Unique high port per call within the process (tests run sequentially in
    // one process, so wrap-around reuse is harmless).
    return "localhost:" + std::to_string(49200 + (n % 16000));
#else
    return (std::filesystem::temp_directory_path() /
            ("lci_test_srv_" + std::to_string(n) + ".sock"))
        .string();
#endif
}

// Raw httplib client for a test server address: TCP on Windows, AF_UNIX on
// POSIX, with test-appropriate timeouts. Mirrors lci::Client's own transport
// selection so fixtures that use httplib directly behave the same.
inline httplib::Client make_test_http_client(const std::string& addr) {
#ifdef _WIN32
    httplib::Client cli("http://" + addr);
#else
    httplib::Client cli(addr);
    cli.set_address_family(AF_UNIX);
#endif
    cli.set_connection_timeout(std::chrono::seconds{5});
    cli.set_read_timeout(std::chrono::seconds{5});
    return cli;
}

}  // namespace test
}  // namespace lci
