#pragma once

#include <cstdint>
#include <mutex>
#include <string>

enum class ControllerState { Idle, Charging, Faulted };

struct ControllerSnapshot {
    std::string id;
    double maximum_power_kw;
    ControllerState state;
    std::string vehicle_id;
    double allocated_power_kw;
    std::string fault_kind;
    std::uint64_t fault_count;
};

class Controller {
  public:
    Controller(std::string id, double maximum_power_kw);

    bool start_session(const std::string& vehicle_id);
    bool stop_session();
    double allocate_power(double requested_power_kw);
    void report_network_fault(const std::string& kind);
    bool recover();
    ControllerState state() const;
    ControllerSnapshot snapshot() const;

  private:
    const std::string id_;
    const double maximum_power_kw_;
    mutable std::mutex mutex_;
    ControllerState state_{ControllerState::Idle};
    std::string vehicle_id_;
    double allocated_power_kw_{0.0};
    std::string fault_kind_;
    std::uint64_t fault_count_{0};
};

std::string controller_state_name(ControllerState state);
