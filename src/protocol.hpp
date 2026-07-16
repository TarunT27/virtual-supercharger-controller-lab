#pragma once

#include "controller.hpp"

#include <chrono>
#include <string>
#include <string_view>

struct ProtocolResult {
    std::string response;
    std::chrono::milliseconds delay{0};
    bool close_connection{false};
    bool corrupt_response{false};
    bool shutdown_server{false};
};

ProtocolResult handle_request(Controller& controller, std::string_view request_line);
