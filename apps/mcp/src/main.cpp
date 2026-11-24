#include "iota/mcp/live_bridge.hpp"
#include "iota/mcp/mcp_server.hpp"
#include "iota/mcp/studio_session.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
  bool attachOnStart = false;
  std::string pipeName;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--version") {
      std::cout << "iota-studio-mcp " << IOTA_STUDIO_VERSION << "\n";
      return 0;
    }
    if (arg == "--attach") {
      attachOnStart = true;
    } else if (arg == "--pipe" && i + 1 < argc) {
      pipeName = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cerr << "Usage: iota_mcp [--version] [--attach] [--pipe <named_pipe>]\n";
      return 0;
    }
  }

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  try {
    iota::mcp::HeadlessStudioSession session;
    iota::mcp::LiveBridgeClient liveClient;
    if (attachOnStart) {
      std::string err;
      if (liveClient.connect(pipeName, 1000, &err)) {
        std::cerr << "[iota-mcp] Attached to running Iota Studio: "
                  << liveClient.connectedPipeName() << "\n";
      } else {
        std::cerr << "[iota-mcp] --attach: " << err
                  << " (starting in standalone headless mode)\n";
      }
    }

    iota::mcp::McpServer server(session, &liveClient);
    return server.runStdioLoop(std::cin, std::cout);
  } catch (const std::exception& ex) {
    std::cerr << "[iota-mcp] Fatal exception: " << ex.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "[iota-mcp] Fatal unknown exception\n";
    return 1;
  }
}
