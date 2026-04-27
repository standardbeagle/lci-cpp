#include <lci/cli/commands.h>
#include <lci/mcp/server.h>
#include <lci/server/server.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace lci {
namespace cli {

int run_mcp(const GlobalFlags& flags) {
    Config cfg;
    if (std::string err = load_config_with_overrides(flags, cfg); !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    // Start MCP server using the existing McpServer class
    mcp::McpServer mcp_server(cfg);
    mcp_server.register_tools();

    // Start a shared IndexServer so CLI commands can also connect
    IndexServer index_server(cfg);
    std::string socket_path = get_socket_path_for_root(cfg.project.root);
    index_server.set_socket_path(socket_path);

    bool shared_server_started = index_server.start();
    if (!shared_server_started) {
        std::cerr << "Warning: failed to start shared index server; "
                     "CLI commands won't be able to connect\n";
    }

    int exit_code = mcp_server.run();

    if (shared_server_started) {
        index_server.shutdown(std::chrono::milliseconds(5000));
    }

    return exit_code;
}

}  // namespace cli
}  // namespace lci
