#pragma once

#include "controller.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct ServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t maximum_frame_bytes{65'536};
};

class TcpServer {
  public:
    TcpServer(Controller& controller, ServerConfig config);
    int run();
    void request_stop();

  private:
    Controller& controller_;
    const ServerConfig config_;
    std::atomic<bool> stop_requested_{false};
};
