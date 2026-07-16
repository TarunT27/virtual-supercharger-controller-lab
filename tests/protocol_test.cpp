#include "controller.hpp"
#include "protocol.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace {

using json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

json response_json(const ProtocolResult& result) {
    return json::parse(result.response);
}

void test_status_and_session_commands() {
    Controller controller{"charger-protocol", 120.0};

    auto result = handle_request(controller, R"({"version":1,"request_id":"r1","command":"status"})");
    auto response = response_json(result);
    require(response.at("success") == true, "status must succeed");
    require(response.at("request_id") == "r1", "request id must be echoed");
    require(response.at("data").at("state") == "idle", "initial state must be idle");

    result = handle_request(
        controller,
        R"({"version":1,"request_id":"r2","command":"start_session","vehicle_id":"vehicle-1"})");
    require(response_json(result).at("success") == true, "start_session must succeed");

    result = handle_request(
        controller,
        R"({"version":1,"request_id":"r3","command":"allocate_power","requested_power_kw":160})");
    response = response_json(result);
    require(response.at("data").at("allocated_power_kw") == 120.0, "allocation must be capped");

    result = handle_request(controller, R"({"version":1,"request_id":"r4","command":"stop_session"})");
    require(response_json(result).at("data").at("state") == "idle", "stop must return idle state");
}

void test_protocol_validation() {
    Controller controller{"charger-validation", 150.0};

    auto response = response_json(handle_request(controller, "not-json"));
    require(response.at("success") == false, "malformed JSON must fail");
    require(response.at("error").at("code") == "invalid_json", "malformed JSON needs a stable code");

    response = response_json(handle_request(
        controller, R"({"version":2,"request_id":"r1","command":"status"})"));
    require(response.at("error").at("code") == "unsupported_version", "version must be validated");

    response = response_json(handle_request(
        controller,
        R"({"version":4294967297,"request_id":"wrapped","command":"status"})"));
    require(response.at("error").at("code") == "unsupported_version",
            "wide versions must not wrap into the supported version");

    response = response_json(handle_request(
        controller,
        R"({"version":1,"request_id":"overflow","command":"allocate_power","requested_power_kw":1e400})"));
    require(response.at("error").at("code") == "invalid_json",
            "numeric overflow must not terminate the daemon");

    response = response_json(handle_request(
        controller, R"({"version":1,"request_id":"r2","command":"unknown"})"));
    require(response.at("error").at("code") == "unknown_command", "commands must be allow-listed");

    response = response_json(handle_request(
        controller, R"({"version":1,"request_id":"r3","command":"start_session"})"));
    require(response.at("error").at("code") == "invalid_request", "required fields must be checked");

    response = response_json(handle_request(
        controller,
        R"({"version":1,"request_id":"wide-delay","command":"inject_fault","kind":"delay","duration_ms":4294967296})"));
    require(response.at("error").at("code") == "invalid_request",
            "wide durations must be rejected before narrowing");
}

void test_fault_transport_actions() {
    Controller controller{"charger-fault", 150.0};
    require(controller.start_session("vehicle-fault"), "session must start");

    auto result = handle_request(
        controller,
        R"({"version":1,"request_id":"delay","command":"inject_fault","kind":"delay","duration_ms":25})");
    require(result.delay == std::chrono::milliseconds{25}, "delay action must be returned");
    require(!result.close_connection, "delay must keep connection open");
    require(response_json(result).at("success") == true, "delay request must acknowledge");
    require(controller.snapshot().state == ControllerState::Faulted, "fault must fail safe");

    require(controller.recover(), "controller must recover");
    result = handle_request(
        controller,
        R"({"version":1,"request_id":"disconnect","command":"inject_fault","kind":"disconnect","duration_ms":0})");
    require(result.close_connection, "disconnect must close the connection before acknowledgment");

    require(controller.recover(), "controller must recover again");
    result = handle_request(
        controller,
        R"({"version":1,"request_id":"corrupt","command":"inject_fault","kind":"corrupt","duration_ms":0})");
    require(result.corrupt_response, "corruption must be observable at the transport boundary");

    result = handle_request(
        controller, R"({"version":1,"request_id":"shutdown","command":"shutdown"})");
    require(result.shutdown_server, "shutdown command must stop the server");
}

}  // namespace

int main() {
    try {
        test_status_and_session_commands();
        test_protocol_validation();
        test_fault_transport_actions();
        std::cout << "protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol test failure: " << error.what() << '\n';
        return 1;
    }
}
