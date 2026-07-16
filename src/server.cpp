#include "server.hpp"

#include "protocol.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket_handle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle invalid_socket_handle = -1;
#endif

namespace {

void close_socket(SocketHandle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

class SocketRuntime {
  public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error{"failed to initialize Winsock"};
        }
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

bool send_all(SocketHandle socket, const std::string& payload) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto remaining = payload.size() - sent;
#ifdef _WIN32
        const int result = send(socket, payload.data() + sent, static_cast<int>(remaining), 0);
#else
        const auto result = send(socket, payload.data() + sent, remaining, MSG_NOSIGNAL);
#endif
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void set_receive_timeout(SocketHandle socket) {
#ifdef _WIN32
    const DWORD timeout_ms = 2'000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
#else
    const timeval timeout{2, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

bool wait_for_connection(SocketHandle listener) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);
    timeval timeout{0, 200'000};
#ifdef _WIN32
    return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
    return select(listener + 1, &read_set, nullptr, nullptr, &timeout) > 0;
#endif
}

bool handle_connection(SocketHandle client, Controller& controller, std::size_t maximum_frame_bytes,
                       std::atomic<bool>& stop_requested) {
    set_receive_timeout(client);
    const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    std::string buffer;
    buffer.reserve(4096);
    char chunk[4096];

    while (!stop_requested.load()) {
        if (std::chrono::steady_clock::now() >= read_deadline) {
            return false;
        }
#ifdef _WIN32
        const int received = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
        const auto received = recv(client, chunk, sizeof(chunk), 0);
#endif
        if (received <= 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= read_deadline) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        if (buffer.size() > maximum_frame_bytes) {
            return false;
        }

        std::size_t newline = 0;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.empty()) {
                return false;
            }

            const ProtocolResult result = handle_request(controller, line);
            if (result.delay.count() > 0) {
                std::this_thread::sleep_for(result.delay);
            }
            if (result.close_connection) {
                return true;
            }
            const std::string payload = result.corrupt_response ? "{corrupt\n" : result.response + "\n";
            if (!send_all(client, payload)) {
                return true;
            }
            if (result.shutdown_server) {
                stop_requested.store(true);
            }
            return true;
        }
    }
    return true;
}

void run_state_worker(std::stop_token stop_token, const Controller& controller) {
    ControllerState previous_state = controller.state();
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const ControllerState current_state = controller.state();
        if (current_state != previous_state) {
            std::osyncstream{std::cout}
                << "{\"worker\":\"state\",\"controller_id\":\""
                << controller.snapshot().id << "\",\"from\":\""
                << controller_state_name(previous_state) << "\",\"to\":\""
                << controller_state_name(current_state) << "\"}\n";
            previous_state = current_state;
        }
    }
}

void run_telemetry_worker(std::stop_token stop_token, const Controller& controller) {
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        const ControllerSnapshot snapshot = controller.snapshot();
        std::osyncstream{std::cout}
            << "{\"worker\":\"telemetry\",\"controller_id\":\"" << snapshot.id
            << "\",\"state\":\"" << controller_state_name(snapshot.state)
            << "\",\"allocated_power_kw\":" << snapshot.allocated_power_kw
            << ",\"fault_count\":" << snapshot.fault_count << "}\n";
    }
}

}  // namespace

TcpServer::TcpServer(Controller& controller, ServerConfig config)
    : controller_(controller), config_(std::move(config)) {
    if (config_.host != "127.0.0.1") {
        throw std::invalid_argument{"host must be 127.0.0.1"};
    }
    if (config_.port == 0 || config_.maximum_frame_bytes < 256 ||
        config_.maximum_frame_bytes > 1'048'576) {
        throw std::invalid_argument{"invalid server configuration"};
    }
}

int TcpServer::run() {
    [[maybe_unused]] SocketRuntime socket_runtime;
    std::jthread state_worker{run_state_worker, std::ref(controller_)};
    std::jthread telemetry_worker{run_telemetry_worker, std::ref(controller_)};
    const SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket_handle) {
        std::cerr << "failed to create TCP socket\n";
        return 2;
    }

#ifdef _WIN32
    const BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1 ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 16) != 0) {
        std::cerr << "failed to bind " << config_.host << ':' << config_.port << '\n';
        close_socket(listener);
        return 3;
    }

    std::cout << "{\"event\":\"ready\",\"worker\":\"network\",\"controller_id\":\""
              << controller_.snapshot().id << "\",\"host\":\"" << config_.host
              << "\",\"port\":" << config_.port << "}\n";
    std::cout.flush();

    while (!stop_requested_.load()) {
        if (!wait_for_connection(listener)) {
            continue;
        }
        sockaddr_in client_address{};
#ifdef _WIN32
        int address_length = sizeof(client_address);
#else
        socklen_t address_length = sizeof(client_address);
#endif
        const SocketHandle client =
            accept(listener, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (client == invalid_socket_handle) {
            continue;
        }
        handle_connection(client, controller_, config_.maximum_frame_bytes, stop_requested_);
        close_socket(client);
    }

    close_socket(listener);
    state_worker.request_stop();
    telemetry_worker.request_stop();
    return 0;
}

void TcpServer::request_stop() {
    stop_requested_.store(true);
}
