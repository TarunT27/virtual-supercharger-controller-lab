#include "controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::size_t maximum_identifier_length = 128;

bool is_valid_identifier(const std::string& value) {
    return !value.empty() && value.size() <= maximum_identifier_length &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' ||
                      character == '.' || character == ':';
           });
}

}  // namespace

Controller::Controller(std::string id, double maximum_power_kw)
    : id_(std::move(id)), maximum_power_kw_(maximum_power_kw) {
    if (!is_valid_identifier(id_)) {
        throw std::invalid_argument{
            "controller id must contain 1-128 letters, numbers, '.', ':', '_' or '-'"};
    }
    if (!std::isfinite(maximum_power_kw_) || maximum_power_kw_ <= 0.0 ||
        maximum_power_kw_ > 1000.0) {
        throw std::invalid_argument{"maximum power must be finite and between 0 and 1000 kW"};
    }
}

bool Controller::start_session(const std::string& vehicle_id) {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Idle || !is_valid_identifier(vehicle_id)) {
        return false;
    }
    vehicle_id_ = vehicle_id;
    allocated_power_kw_ = 0.0;
    state_ = ControllerState::Charging;
    return true;
}

bool Controller::stop_session() {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Charging) {
        return false;
    }
    vehicle_id_.clear();
    allocated_power_kw_ = 0.0;
    state_ = ControllerState::Idle;
    return true;
}

double Controller::allocate_power(double requested_power_kw) {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Charging || !std::isfinite(requested_power_kw) ||
        requested_power_kw <= 0.0) {
        return 0.0;
    }
    allocated_power_kw_ = std::min(requested_power_kw, maximum_power_kw_);
    return allocated_power_kw_;
}

void Controller::report_network_fault(const std::string& kind) {
    std::scoped_lock lock{mutex_};
    state_ = ControllerState::Faulted;
    allocated_power_kw_ = 0.0;
    fault_kind_ = kind;
    ++fault_count_;
}

bool Controller::recover() {
    std::scoped_lock lock{mutex_};
    if (state_ != ControllerState::Faulted) {
        return false;
    }
    vehicle_id_.clear();
    allocated_power_kw_ = 0.0;
    fault_kind_.clear();
    state_ = ControllerState::Idle;
    return true;
}

ControllerState Controller::state() const {
    std::scoped_lock lock{mutex_};
    return state_;
}

ControllerSnapshot Controller::snapshot() const {
    std::scoped_lock lock{mutex_};
    return ControllerSnapshot{id_,          maximum_power_kw_, state_,
                              vehicle_id_, allocated_power_kw_, fault_kind_, fault_count_};
}

std::string controller_state_name(ControllerState state) {
    switch (state) {
        case ControllerState::Idle:
            return "idle";
        case ControllerState::Charging:
            return "charging";
        case ControllerState::Faulted:
            return "faulted";
    }
    return "unknown";
}
