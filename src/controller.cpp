#include "controller.hpp"

#include <algorithm>
#include <utility>

Controller::Controller(std::string id, double maximum_power_kw)
    : id_(std::move(id)), maximum_power_kw_(maximum_power_kw) {}

bool Controller::start_session(const std::string& vehicle_id) {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Idle || vehicle_id.empty()) {
        return false;
    }
    vehicle_id_ = vehicle_id;
    state_ = ControllerState::Charging;
    return true;
}

double Controller::allocate_power(double requested_power_kw) {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Charging || requested_power_kw <= 0.0) {
        return 0.0;
    }
    return std::min(requested_power_kw, maximum_power_kw_);
}

void Controller::report_network_fault() {
    std::scoped_lock lock{mutex_};
    state_ = ControllerState::Faulted;
}

bool Controller::recover() {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Faulted) {
        return false;
    }
    vehicle_id_.clear();
    state_ = ControllerState::Idle;
    return true;
}

ControllerState Controller::state() const {
    std::scoped_lock lock{mutex_};
    return state_;
}
