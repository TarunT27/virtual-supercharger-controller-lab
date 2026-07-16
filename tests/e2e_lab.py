"""Cross-language end-to-end test, invoked by CTest with a built controller binary."""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from orchestrator import (
    MAX_FRAME_BYTES,
    ControllerEndpoint,
    run_demo,
    send_request,
    terminate_controllers,
    wait_until_ready,
)


def verify_oversized_frame_isolated(binary: str) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    endpoint = ControllerEndpoint("127.0.0.1", port, "oversized-probe")
    process = subprocess.Popen(  # noqa: S603 - CTest supplies the built local binary
        (
            binary,
            "--host",
            endpoint.host,
            "--id",
            endpoint.controller_id,
            "--port",
            str(endpoint.port),
        ),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        wait_until_ready(endpoint)
        with socket.create_connection((endpoint.host, endpoint.port), timeout=2.0) as connection:
            connection.sendall(b"x" * (MAX_FRAME_BYTES + 1) + b"\n")
            try:
                received = connection.recv(1)
            except ConnectionResetError:
                received = b""
        if received:
            raise RuntimeError("oversized frame received an unexpected response")
        if send_request(endpoint, {"command": "health"}).get("success") is not True:
            raise RuntimeError("server did not remain healthy after oversized input")
        send_request(endpoint, {"command": "shutdown"})
        process.wait(timeout=2.0)
    finally:
        terminate_controllers((process,))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()

    verify_oversized_frame_isolated(args.binary)
    report = run_demo(count=3, executable=args.binary, start_port=19000)
    if report["controllers_started"] != 3:
        raise RuntimeError("demo did not start the requested fleet")
    if not all(item["recovered_state"] == "idle" for item in report["controllers"]):
        raise RuntimeError("not every controller recovered to idle")
    if not all(item["session_stopped"] for item in report["controllers"]):
        raise RuntimeError("not every controller completed a post-recovery session")
    if not all(item["fault_observed"] for item in report["controllers"]):
        raise RuntimeError("not every transport fault was observed by the Python client")
    if {item["fault_kind"] for item in report["controllers"]} != {
        "delay",
        "disconnect",
        "corrupt",
    }:
        raise RuntimeError("demo did not exercise all supported faults")
    print("end-to-end lab scenario passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
