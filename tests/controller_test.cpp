#include "controller.hpp"

#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

template <typename Callable>
void require_invalid_argument(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error{message};
}

void test_configuration_validation() {
    require_invalid_argument([] { Controller{"", 150.0}; }, "blank ids must be rejected");
    require_invalid_argument([] { Controller{"bad\"id", 150.0}; },
                             "ids that can corrupt structured logs must be rejected");
    require_invalid_argument([] { Controller{"charger-1", 0.0}; }, "zero power must be rejected");
    require_invalid_argument([] { Controller{"charger-1", -1.0}; }, "negative power must be rejected");
    require_invalid_argument(
        [] { Controller{"charger-1", std::numeric_limits<double>::quiet_NaN()}; },
        "NaN power must be rejected");
}

void test_session_lifecycle_and_snapshot() {
    Controller controller{"charger-1", 150.0};

    auto snapshot = controller.snapshot();
    require(snapshot.id == "charger-1", "snapshot must expose controller id");
    require(snapshot.state == ControllerState::Idle, "controller must start idle");
    require(!controller.start_session(""), "blank vehicle must be rejected");
    require(controller.start_session("vehicle-42"), "first session must start");
    require(!controller.start_session("vehicle-43"), "second session must be rejected");
    require(controller.allocate_power(75.0) == 75.0, "power below limit must be allocated");
    require(controller.allocate_power(200.0) == 150.0, "power must be capped");

    snapshot = controller.snapshot();
    require(snapshot.state == ControllerState::Charging, "controller must be charging");
    require(snapshot.vehicle_id == "vehicle-42", "active vehicle must be captured");
    require(snapshot.allocated_power_kw == 150.0, "latest allocation must be captured");
    require(controller.stop_session(), "active session must stop");
    require(!controller.stop_session(), "idle session cannot stop twice");

    snapshot = controller.snapshot();
    require(snapshot.state == ControllerState::Idle, "stopped controller must be idle");
    require(snapshot.vehicle_id.empty(), "stopped session must clear vehicle");
    require(snapshot.allocated_power_kw == 0.0, "stopped session must clear allocation");
}

void test_power_boundaries_and_fault_recovery() {
    Controller controller{"charger-2", 100.0};
    require(controller.allocate_power(50.0) == 0.0, "idle controller must not allocate");
    require(controller.start_session("vehicle-7"), "session must start");
    require(controller.allocate_power(0.0) == 0.0, "zero request must be rejected");
    require(controller.allocate_power(-5.0) == 0.0, "negative request must be rejected");
    require(controller.allocate_power(std::numeric_limits<double>::infinity()) == 0.0,
            "infinite request must be rejected");
    require(controller.allocate_power(std::numeric_limits<double>::quiet_NaN()) == 0.0,
            "NaN request must be rejected");

    controller.report_network_fault("disconnect");
    const auto faulted = controller.snapshot();
    require(faulted.state == ControllerState::Faulted, "fault must transition state");
    require(faulted.fault_kind == "disconnect", "fault type must be observable");
    require(faulted.fault_count == 1, "fault counter must increment");
    require(faulted.allocated_power_kw == 0.0, "fault must fail safe");
    require(controller.allocate_power(50.0) == 0.0, "faulted controller must not allocate");
    require(controller.recover(), "faulted controller must recover");
    require(!controller.recover(), "idle controller must not recover twice");

    const auto recovered = controller.snapshot();
    require(recovered.state == ControllerState::Idle, "recovery must return to idle");
    require(recovered.vehicle_id.empty(), "recovery must clear the session");
    require(recovered.fault_kind.empty(), "recovery must clear active fault");
}

void test_concurrent_reads_and_allocations() {
    Controller controller{"charger-concurrent", 350.0};
    require(controller.start_session("fleet-vehicle"), "session must start");
    std::atomic<bool> failed{false};
    std::vector<std::jthread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&controller, &failed] {
            for (int iteration = 0; iteration < 500; ++iteration) {
                const double allocated = controller.allocate_power(175.0);
                const auto snapshot = controller.snapshot();
                if (allocated != 175.0 || snapshot.id != "charger-concurrent") {
                    failed.store(true);
                }
            }
        });
    }
    workers.clear();
    require(!failed.load(), "concurrent operations must remain consistent");
}

}  // namespace

int main() {
    try {
        test_configuration_validation();
        test_session_lifecycle_and_snapshot();
        test_power_boundaries_and_fault_recovery();
        test_concurrent_reads_and_allocations();
        std::cout << "controller tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "controller test failure: " << error.what() << '\n';
        return 1;
    }
}
