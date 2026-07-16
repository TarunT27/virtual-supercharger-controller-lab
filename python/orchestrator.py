"""Control plane for the virtual supercharger controller lab.

The module deliberately uses only the Python standard library so the lab can be
run immediately after building the C++ controller.  Requests and responses use
bounded, newline-delimited JSON frames over one TCP connection per request.
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import subprocess
import time
import uuid
from collections.abc import Mapping, Sequence
from contextlib import suppress
from dataclasses import dataclass
from typing import Any

PROTOCOL_VERSION = 1
MAX_FRAME_BYTES = 64 * 1024
MAX_CONTROLLERS = 100
MAX_FAULT_DURATION_MS = 30_000
MAXIMUM_POWER_KW = 1_000.0
DEFAULT_SOCKET_TIMEOUT_SECONDS = 2.0


class ProtocolError(RuntimeError):
    """Raised when a peer violates the controller wire protocol."""


@dataclass(frozen=True, slots=True)
class ControllerEndpoint:
    """Immutable network identity for one virtual controller."""

    host: str
    port: int
    controller_id: str

    def __post_init__(self) -> None:
        _validate_nonempty_text("host", self.host)
        _validate_port(self.port)
        _validate_nonempty_text("controller_id", self.controller_id)


def _validate_nonempty_text(name: str, value: object) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{name} must be a non-empty string")
    if "\x00" in value:
        raise ValueError(f"{name} cannot contain a null byte")
    return value


def _validate_port(port: object) -> int:
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65_535:
        raise ValueError("port must be an integer between 1 and 65535")
    return port


def _validate_positive_number(name: str, value: object) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be a positive finite number")
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"{name} must be a positive finite number")
    return number


def build_controller_commands(
    count: int,
    host: str,
    start_port: int,
    executable: str = "./build/controller_lab",
    maximum_power_kw: float = 150.0,
) -> tuple[tuple[str, ...], ...]:
    """Build validated command lines without launching or mutating anything."""

    if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_CONTROLLERS:
        raise ValueError(f"count must be between 1 and {MAX_CONTROLLERS}")
    _validate_nonempty_text("host", host)
    if host != "127.0.0.1":
        raise ValueError("host must be 127.0.0.1")
    _validate_port(start_port)
    if start_port + count - 1 > 65_535:
        raise ValueError("controller port range exceeds 65535")
    _validate_nonempty_text("executable", executable)
    maximum_power = _validate_positive_number("maximum_power_kw", maximum_power_kw)
    if maximum_power > MAXIMUM_POWER_KW:
        raise ValueError(f"maximum_power_kw cannot exceed {MAXIMUM_POWER_KW}")

    return tuple(
        (
            executable,
            "--host",
            host,
            "--id",
            f"charger-{index}",
            "--port",
            str(start_port + index),
            "--max-power-kw",
            str(maximum_power),
        )
        for index in range(count)
    )


def launch_controllers(
    commands: Sequence[Sequence[str]],
) -> tuple[subprocess.Popen[str], ...]:
    """Launch controllers, rolling back already-started children on failure."""

    processes: list[subprocess.Popen[str]] = []
    try:
        for command in commands:
            immutable_command = tuple(command)
            if not immutable_command:
                raise ValueError("controller command cannot be empty")
            processes.append(
                subprocess.Popen(  # noqa: S603 - validated local command, never a shell
                    immutable_command,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
            )
    except BaseException:
        terminate_controllers(tuple(processes))
        raise
    return tuple(processes)


def terminate_controllers(
    processes: Sequence[subprocess.Popen[str]],
    timeout_seconds: float = 2.0,
) -> None:
    """Terminate every child and escalate to kill after a shared deadline."""

    timeout = _validate_positive_number("timeout_seconds", timeout_seconds)
    immutable_processes = tuple(processes)
    live_processes: tuple[subprocess.Popen[str], ...] = ()
    for process in immutable_processes:
        try:
            if process.poll() is None:
                live_processes = (*live_processes, process)
        except OSError:
            live_processes = (*live_processes, process)

    for process in live_processes:
        with suppress(OSError):
            process.terminate()

    deadline = time.monotonic() + timeout
    for process in live_processes:
        try:
            if process.poll() is None:
                remaining = max(0.0, deadline - time.monotonic())
                process.wait(timeout=remaining)
        except (OSError, subprocess.TimeoutExpired):
            with suppress(OSError):
                process.kill()

    for process in live_processes:
        try:
            if process.poll() is None:
                process.wait(timeout=1.0)
        except (OSError, subprocess.TimeoutExpired):
            continue


def inject_tcp_fault(kind: str, duration_ms: int = 0) -> dict[str, int | str]:
    """Create a validated fault command for the controller protocol."""

    if kind not in {"delay", "disconnect", "corrupt"}:
        raise ValueError(f"unsupported fault type: {kind}")
    if (
        isinstance(duration_ms, bool)
        or not isinstance(duration_ms, int)
        or not 0 <= duration_ms <= MAX_FAULT_DURATION_MS
    ):
        raise ValueError(f"duration_ms must be between 0 and {MAX_FAULT_DURATION_MS}")
    if kind != "delay" and duration_ms != 0:
        raise ValueError("duration_ms must be zero for disconnect and corrupt faults")
    return {"command": "inject_fault", "kind": kind, "duration_ms": duration_ms}


def _request_frame(request: Mapping[str, Any], request_id: str) -> bytes:
    _validate_nonempty_text("request_id", request_id)
    if len(request_id) > 128:
        raise ValueError("request_id cannot exceed 128 characters")
    command = request.get("command")
    _validate_nonempty_text("command", command)
    payload = {**dict(request), "version": PROTOCOL_VERSION, "request_id": request_id}
    try:
        frame = json.dumps(payload, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"
    except (TypeError, ValueError) as error:
        raise ValueError("request must contain only JSON-compatible values") from error
    if len(frame) > MAX_FRAME_BYTES:
        raise ValueError(f"request exceeds the {MAX_FRAME_BYTES}-byte frame limit")
    return frame


def _receive_frame(connection: socket.socket) -> bytes:
    received = bytearray()
    while len(received) < MAX_FRAME_BYTES:
        chunk = connection.recv(min(4096, MAX_FRAME_BYTES - len(received)))
        if not chunk:
            raise ProtocolError("controller closed the connection before completing a response")
        received.extend(chunk)
        newline = received.find(b"\n")
        if newline >= 0:
            if received[newline + 1 :]:
                raise ProtocolError("controller returned data after the response frame")
            return bytes(received[:newline])
    raise ProtocolError(f"response exceeds the {MAX_FRAME_BYTES}-byte frame limit")


def _decode_response(frame: bytes, endpoint: ControllerEndpoint, request_id: str) -> dict[str, Any]:
    try:
        decoded = json.loads(frame.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProtocolError("controller returned invalid JSON") from error
    if not isinstance(decoded, dict):
        raise ProtocolError("controller response must be a JSON object")
    if decoded.get("version") != PROTOCOL_VERSION:
        raise ProtocolError("controller returned an unsupported protocol version")
    if decoded.get("request_id") != request_id:
        raise ProtocolError("controller response request_id does not match the request")
    if decoded.get("controller_id") != endpoint.controller_id:
        raise ProtocolError("controller response came from an unexpected controller")
    if not isinstance(decoded.get("success"), bool):
        raise ProtocolError("controller response is missing a boolean success field")
    if decoded["success"]:
        if not isinstance(decoded.get("data"), dict) or decoded.get("error") is not None:
            raise ProtocolError("successful response must contain object data and no error")
    else:
        error_payload = decoded.get("error")
        if (
            decoded.get("data") is not None
            or not isinstance(error_payload, dict)
            or not isinstance(error_payload.get("code"), str)
            or not isinstance(error_payload.get("message"), str)
        ):
            raise ProtocolError("failed response must contain a structured error and no data")
    return decoded


def send_request(
    endpoint: ControllerEndpoint,
    request: Mapping[str, Any],
    *,
    request_id: str | None = None,
    timeout_seconds: float = DEFAULT_SOCKET_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Send one request and return one validated response envelope."""

    if not isinstance(endpoint, ControllerEndpoint):
        raise TypeError("endpoint must be a ControllerEndpoint")
    if not isinstance(request, Mapping):
        raise TypeError("request must be a mapping")
    timeout = _validate_positive_number("timeout_seconds", timeout_seconds)
    actual_request_id = uuid.uuid4().hex if request_id is None else request_id
    frame = _request_frame(request, actual_request_id)

    with socket.create_connection((endpoint.host, endpoint.port), timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(frame)
        response_frame = _receive_frame(connection)
    return _decode_response(response_frame, endpoint, actual_request_id)


def send_fault(
    endpoint: ControllerEndpoint,
    fault: Mapping[str, Any],
    *,
    request_id: str | None = None,
    timeout_seconds: float = DEFAULT_SOCKET_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Inject a fault through the same validated protocol used for commands."""

    if request_id is not None and timeout_seconds == DEFAULT_SOCKET_TIMEOUT_SECONDS:
        return send_request(endpoint, fault, request_id=request_id)
    return send_request(
        endpoint,
        fault,
        request_id=request_id,
        timeout_seconds=timeout_seconds,
    )


def wait_until_ready(
    endpoint: ControllerEndpoint,
    *,
    timeout_seconds: float = 5.0,
    poll_interval_seconds: float = 0.05,
) -> None:
    """Poll the health command until it succeeds or a bounded deadline passes."""

    timeout = _validate_positive_number("timeout_seconds", timeout_seconds)
    poll_interval = _validate_positive_number("poll_interval_seconds", poll_interval_seconds)
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            response = send_request(
                endpoint,
                {"command": "health"},
                timeout_seconds=max(0.001, min(DEFAULT_SOCKET_TIMEOUT_SECONDS, remaining)),
            )
            if response.get("success") is True:
                return
            last_error = ProtocolError("controller health request was unsuccessful")
        except (OSError, ProtocolError) as error:
            last_error = error
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(poll_interval, remaining))
    raise TimeoutError(f"controller {endpoint.controller_id} did not become ready") from last_error


def _require_success(response: Mapping[str, Any], operation: str) -> Mapping[str, Any]:
    if response.get("success") is not True:
        error = response.get("error")
        raise ProtocolError(f"{operation} failed: {error}")
    data = response.get("data")
    return data if isinstance(data, Mapping) else {}


def run_demo(
    *,
    count: int = 3,
    host: str = "127.0.0.1",
    start_port: int = 9000,
    executable: str = "./build/controller_lab",
    maximum_power_kw: float = 150.0,
) -> dict[str, Any]:
    """Run a deterministic multi-controller fault and recovery scenario."""

    commands = build_controller_commands(
        count=count,
        host=host,
        start_port=start_port,
        executable=executable,
        maximum_power_kw=maximum_power_kw,
    )
    endpoints = tuple(
        ControllerEndpoint(host, start_port + index, f"charger-{index}") for index in range(count)
    )
    processes = launch_controllers(commands)
    ready_endpoints: list[ControllerEndpoint] = []
    controller_reports: list[dict[str, Any]] = []
    fault_kinds = ("delay", "disconnect", "corrupt")

    try:
        for endpoint, process in zip(endpoints, processes, strict=True):
            wait_until_ready(endpoint)
            return_code = process.poll()
            if return_code is not None:
                raise RuntimeError(
                    f"controller process {endpoint.controller_id} exited with code "
                    f"{return_code}; refusing to use an unowned endpoint"
                )
            ready_endpoints.append(endpoint)

        for index, endpoint in enumerate(endpoints):
            _require_success(
                send_request(
                    endpoint,
                    {"command": "start_session", "vehicle_id": f"demo-vehicle-{index}"},
                ),
                "start_session",
            )
            allocation = _require_success(
                send_request(
                    endpoint,
                    {
                        "command": "allocate_power",
                        "requested_power_kw": 80.0 + index * 10.0,
                    },
                ),
                "allocate_power",
            )

            fault_kind = fault_kinds[index % len(fault_kinds)]
            fault_duration_ms = 25 if fault_kind == "delay" else 0
            fault = inject_tcp_fault(fault_kind, fault_duration_ms)
            fault_started = time.monotonic()
            try:
                fault_response = send_fault(endpoint, fault)
                _require_success(fault_response, "inject_fault")
            except (OSError, ProtocolError):
                if fault_kind == "delay":
                    raise
                fault_observed = True
            else:
                if fault_kind != "delay":
                    raise ProtocolError(
                        f"{endpoint.controller_id} did not expose the {fault_kind} transport fault"
                    )
                elapsed_seconds = time.monotonic() - fault_started
                minimum_delay_seconds = fault_duration_ms / 1000.0 - 0.005
                if elapsed_seconds < minimum_delay_seconds:
                    raise ProtocolError(f"{endpoint.controller_id} delay fault returned too early")
                fault_observed = True

            recovery = _require_success(
                send_request(endpoint, {"command": "recover"}),
                "recover",
            )
            recovered_state = recovery.get("state")
            if recovered_state is None:
                status = _require_success(
                    send_request(endpoint, {"command": "status"}),
                    "status",
                )
                recovered_state = status.get("state")
            if recovered_state != "idle":
                raise ProtocolError(f"{endpoint.controller_id} did not recover to idle")

            _require_success(
                send_request(
                    endpoint,
                    {
                        "command": "start_session",
                        "vehicle_id": f"post-recovery-vehicle-{index}",
                    },
                ),
                "post_recovery_start_session",
            )
            stopped = _require_success(
                send_request(endpoint, {"command": "stop_session"}),
                "post_recovery_stop_session",
            )
            session_stopped = stopped.get("state") == "idle"
            if not session_stopped:
                raise ProtocolError(
                    f"{endpoint.controller_id} did not complete a post-recovery session"
                )

            controller_reports.append(
                {
                    "controller_id": endpoint.controller_id,
                    "port": endpoint.port,
                    "allocated_power_kw": allocation.get("allocated_power_kw"),
                    "fault_kind": fault_kind,
                    "fault_observed": fault_observed,
                    "recovered_state": recovered_state,
                    "session_stopped": session_stopped,
                }
            )

        return {
            "controllers_started": len(ready_endpoints),
            "controllers": controller_reports,
        }
    finally:
        for endpoint in tuple(ready_endpoints):
            with suppress(OSError, ProtocolError):
                send_request(endpoint, {"command": "shutdown"}, timeout_seconds=0.5)
        for process in processes:
            with suppress(OSError, subprocess.TimeoutExpired):
                process.wait(timeout=0.5)
        terminate_controllers(processes)


def _build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the virtual supercharger controller lab")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--start-port", type=int, default=9000)
    parser.add_argument("--executable", default="./build/controller_lab")
    parser.add_argument("--maximum-power-kw", type=float, default=150.0)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print validated controller commands without launching processes",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_argument_parser().parse_args(argv)
    if args.dry_run:
        commands = build_controller_commands(
            count=args.count,
            host=args.host,
            start_port=args.start_port,
            executable=args.executable,
            maximum_power_kw=args.maximum_power_kw,
        )
        print(json.dumps(commands, indent=2))
        return 0

    report = run_demo(
        count=args.count,
        host=args.host,
        start_port=args.start_port,
        executable=args.executable,
        maximum_power_kw=args.maximum_power_kw,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
