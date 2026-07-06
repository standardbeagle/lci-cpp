#pragma once

// Cross-platform test server addressing.
//
// On POSIX the IndexServer listens on a Unix-domain socket (a ".sock" path).
// Windows has no AF_UNIX, so production falls back to TCP "127.0.0.1:<port>"
// (see get_socket_path() / server.cpp). A test fixture must therefore hand the
// server — and any raw httplib client — an address of the platform-correct
// shape. A Windows temp ".sock" path is "C:\...\x.sock", and the server's port
// parser (stoi after the last ':') hits the drive-letter colon and throws in
// SetUp(), failing every test in the fixture. These helpers keep that
// divergence in one place.

#include <atomic>
#include <chrono>
#include <string>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <filesystem>
#include <sys/un.h>
#include <unistd.h>  // getpid
#endif

#include <httplib.h>

namespace lci {
namespace test {

// Process-unique address suitable for IndexServer::set_socket_path().
//
// gtest_discover_tests + `ctest -j` run each TEST case as its own process of
// this binary, concurrently. The address must therefore be unique per PROCESS,
// not just per call: a bare per-process counter starting at 0 handed two
// parallel ServerTest/ClientTest workers the SAME socket path/port, so their
// servers collided on bind/connect → aborts and read-timeouts. Fold in the pid
// (POSIX: socket filename; Windows: port base) so concurrent processes never
// share an address; the atomic counter keeps it unique within a process.
inline std::string next_test_server_address() {
    static std::atomic<int> counter{0};
    int n = counter.fetch_add(1);
#ifdef _WIN32
    // 127.0.0.1, not "localhost": the server binds IPv4 and "localhost"
    // resolves to ::1 first on Windows, yielding connection-refused. Base the
    // port on the pid so two concurrent test processes claim disjoint ranges.
    unsigned pid = static_cast<unsigned>(_getpid());
    int base = 49200 + static_cast<int>((pid * 251u) % 15000u);
    return "127.0.0.1:" + std::to_string(base + (n % 1000));
#else
    return (std::filesystem::temp_directory_path() /
            ("lci_test_srv_" + std::to_string(::getpid()) + "_" +
             std::to_string(n) + ".sock"))
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
