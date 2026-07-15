#include "controller.hpp"

#include <cassert>

int main() {
    Controller controller{"charger-1", 150.0};

    assert(controller.state() == ControllerState::Idle);
    assert(controller.start_session("vehicle-42"));
    assert(controller.state() == ControllerState::Charging);
    assert(controller.allocate_power(75.0) == 75.0);
    assert(controller.allocate_power(200.0) == 150.0);
    controller.report_network_fault();
    assert(controller.state() == ControllerState::Faulted);
    assert(controller.recover());
    assert(controller.state() == ControllerState::Idle);

    return 0;
}
