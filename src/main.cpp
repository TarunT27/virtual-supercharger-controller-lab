#include "controller.hpp"
#include "server.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct ApplicationConfig {
    std::string host{"127.0.0.1"};
    std::string id{"charger-0"};
    std::uint16_t port{9000};
    double maximum_power_kw{150.0};
};

void print_help() {
    std::cout << "Virtual Supercharger Controller Lab\n\n"
              << "Usage: controller_lab [options]\n"
              << "  --host <127.0.0.1>\n"
              << "  --id <controller-id>\n"
              << "  --port <1-65535>\n"
              << "  --max-power-kw <positive-kW>\n";
}

ApplicationConfig parse_arguments(int argc, char** argv) {
    ApplicationConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            print_help();
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument{"missing value for " + argument};
        }
        const std::string value = argv[++index];
        if (argument == "--host") {
            config.host = value;
        } else if (argument == "--id") {
            config.id = value;
        } else if (argument == "--port") {
            std::size_t consumed = 0;
            const unsigned long port = std::stoul(value, &consumed);
            if (consumed != value.size() || port == 0 || port > 65'535) {
                throw std::invalid_argument{"port must be between 1 and 65535"};
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--max-power-kw") {
            std::size_t consumed = 0;
            config.maximum_power_kw = std::stod(value, &consumed);
            if (consumed != value.size()) {
                throw std::invalid_argument{"maximum power must be a number"};
            }
        } else {
            throw std::invalid_argument{"unknown argument: " + argument};
        }
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const ApplicationConfig config = parse_arguments(argc, argv);
        Controller controller{config.id, config.maximum_power_kw};
        TcpServer server{controller, ServerConfig{config.host, config.port}};
        return server.run();
    } catch (const std::exception& error) {
        std::cerr << "controller_lab: " << error.what() << '\n';
        return 1;
    }
}
