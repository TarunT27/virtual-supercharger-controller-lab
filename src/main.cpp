#include "controller.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    Controller controller{"charger-0", 150.0};

    std::jthread state_worker{[&controller] {
        controller.start_session("demo-vehicle");
        std::cout << "state worker: session started\n";
    }};
    std::jthread network_worker{[] {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        std::cout << "network worker: message received\n";
    }};
    std::jthread telemetry_worker{[&controller] {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        std::cout << "telemetry worker: " << controller.allocate_power(80.0) << " kW\n";
    }};

    return 0;
}
