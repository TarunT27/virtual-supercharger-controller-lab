#include "protocol.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using json = nlohmann::json;

constexpr int protocol_version = 1;
constexpr int maximum_delay_ms = 30'000;

json snapshot_json(const ControllerSnapshot& snapshot) {
    return {{"id", snapshot.id},
            {"state", controller_state_name(snapshot.state)},
            {"maximum_power_kw", snapshot.maximum_power_kw},
            {"vehicle_id", snapshot.vehicle_id.empty() ? json{nullptr} : json(snapshot.vehicle_id)},
            {"allocated_power_kw", snapshot.allocated_power_kw},
            {"fault_kind", snapshot.fault_kind.empty() ? json{nullptr} : json(snapshot.fault_kind)},
            {"fault_count", snapshot.fault_count}};
}

json envelope(const ControllerSnapshot& snapshot, const std::string& request_id, bool success,
              json data, json error) {
    return {{"version", protocol_version},
            {"request_id", request_id.empty() ? json{nullptr} : json(request_id)},
            {"success", success},
            {"controller_id", snapshot.id},
            {"data", std::move(data)},
            {"error", std::move(error)}};
}

ProtocolResult failure(const Controller& controller, const std::string& request_id,
                       const std::string& code, const std::string& message) {
    return {envelope(controller.snapshot(), request_id, false, nullptr,
                     {{"code", code}, {"message", message}})
                .dump()};
}

ProtocolResult success(const Controller& controller, const std::string& request_id, json data) {
    return {envelope(controller.snapshot(), request_id, true, std::move(data), nullptr).dump()};
}

bool has_string(const json& request, const char* key) {
    return request.contains(key) && request.at(key).is_string() &&
           !request.at(key).get_ref<const std::string&>().empty();
}

}  // namespace

ProtocolResult handle_request(Controller& controller, std::string_view request_line) {
    json request;
    try {
        request = json::parse(request_line);
    } catch (const json::exception&) {
        return failure(controller, "", "invalid_json", "request must be valid JSON");
    }
    if (!request.is_object()) {
        return failure(controller, "", "invalid_request", "request must be a JSON object");
    }

    const std::string request_id = has_string(request, "request_id")
                                       ? request.at("request_id").get<std::string>()
                                       : "";
    if (request_id.size() > 128) {
        return failure(controller, "", "invalid_request", "request_id is too long");
    }
    if (!request.contains("version") || !request.at("version").is_number_integer() ||
        request.at("version") != protocol_version) {
        return failure(controller, request_id, "unsupported_version",
                       "only protocol version 1 is supported");
    }
    if (request_id.empty() || !has_string(request, "command")) {
        return failure(controller, request_id, "invalid_request",
                       "request_id and command are required strings");
    }

    const std::string command = request.at("command").get<std::string>();
    if (command == "health" || command == "status") {
        return success(controller, request_id, snapshot_json(controller.snapshot()));
    }
    if (command == "start_session") {
        if (!has_string(request, "vehicle_id")) {
            return failure(controller, request_id, "invalid_request", "vehicle_id is required");
        }
        const std::string vehicle_id = request.at("vehicle_id").get<std::string>();
        if (!controller.start_session(vehicle_id)) {
            return failure(controller, request_id, "invalid_state",
                           "controller is not idle or vehicle_id is invalid");
        }
        return success(controller, request_id, snapshot_json(controller.snapshot()));
    }
    if (command == "allocate_power") {
        if (!request.contains("requested_power_kw") ||
            !request.at("requested_power_kw").is_number()) {
            return failure(controller, request_id, "invalid_request",
                           "requested_power_kw must be a number");
        }
        const double requested = request.at("requested_power_kw").get<double>();
        if (!std::isfinite(requested) || requested <= 0.0) {
            return failure(controller, request_id, "invalid_request",
                           "requested_power_kw must be finite and positive");
        }
        const double allocated = controller.allocate_power(requested);
        if (allocated == 0.0) {
            return failure(controller, request_id, "invalid_state",
                           "power can only be allocated while charging");
        }
        json data = snapshot_json(controller.snapshot());
        data["requested_power_kw"] = requested;
        return success(controller, request_id, std::move(data));
    }
    if (command == "stop_session") {
        if (!controller.stop_session()) {
            return failure(controller, request_id, "invalid_state", "no charging session is active");
        }
        return success(controller, request_id, snapshot_json(controller.snapshot()));
    }
    if (command == "recover") {
        if (!controller.recover()) {
            return failure(controller, request_id, "invalid_state", "controller is not faulted");
        }
        return success(controller, request_id, snapshot_json(controller.snapshot()));
    }
    if (command == "inject_fault") {
        if (!has_string(request, "kind") || !request.contains("duration_ms") ||
            !request.at("duration_ms").is_number_integer()) {
            return failure(controller, request_id, "invalid_request",
                           "kind and integer duration_ms are required");
        }
        const std::string kind = request.at("kind").get<std::string>();
        const json& duration_value = request.at("duration_ms");
        const bool duration_in_range =
            duration_value.is_number_unsigned()
                ? duration_value.get<std::uint64_t>() <=
                      static_cast<std::uint64_t>(maximum_delay_ms)
                : duration_value.get<std::int64_t>() >= 0 &&
                      duration_value.get<std::int64_t>() <= maximum_delay_ms;
        if (!duration_in_range) {
            return failure(controller, request_id, "invalid_request",
                           "unsupported fault parameters");
        }
        const int duration_ms = duration_value.get<int>();
        if ((kind != "delay" && kind != "disconnect" && kind != "corrupt") ||
            (kind != "delay" && duration_ms != 0)) {
            return failure(controller, request_id, "invalid_request", "unsupported fault parameters");
        }
        controller.report_network_fault(kind);
        ProtocolResult result = success(controller, request_id, snapshot_json(controller.snapshot()));
        result.delay = std::chrono::milliseconds{duration_ms};
        result.close_connection = kind == "disconnect";
        result.corrupt_response = kind == "corrupt";
        return result;
    }
    if (command == "shutdown") {
        ProtocolResult result = success(controller, request_id, snapshot_json(controller.snapshot()));
        result.shutdown_server = true;
        return result;
    }
    return failure(controller, request_id, "unknown_command", "command is not supported");
}
