#pragma once

#include <mutex>
#include <string>

enum class ControllerState { Idle, Charging, Faulted };

class Controller {
  public:
    Controller(std::string id, double maximum_power_kw);

    bool start_session(const std::string& vehicle_id);
    double allocate_power(double requested_power_kw);
    void report_network_fault();
    bool recover();
    ControllerState state() const;

  private:
    const std::string id_;
    const double maximum_power_kw_;
    mutable std::mutex mutex_;
    ControllerState state_{ControllerState::Idle};
    std::string vehicle_id_;
};
