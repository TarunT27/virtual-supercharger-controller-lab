"""Small process orchestrator and TCP fault-message sender."""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
from collections.abc import Sequence


def build_controller_commands(
    count: int,
    host: str,
    start_port: int,
    executable: str = "./controller_lab",
) -> tuple[tuple[str, ...], ...]:
    if count < 1:
        raise ValueError("count must be at least 1")
    return tuple(
        (executable, host, f"charger-{index}", str(start_port + index))
        for index in range(count)
    )


def launch_controllers(commands: Sequence[Sequence[str]]) -> tuple[subprocess.Popen[str], ...]:
    return tuple(
        subprocess.Popen(tuple(command), text=True)  # noqa: S603 - commands are locally constructed
        for command in commands
    )


def inject_tcp_fault(kind: str, duration_ms: int = 0) -> dict[str, int | str]:
    if kind not in {"delay", "disconnect", "corrupt"}:
        raise ValueError(f"unsupported fault type: {kind}")
    if duration_ms < 0:
        raise ValueError("duration_ms cannot be negative")
    return {"type": kind, "duration_ms": duration_ms}


def send_fault(host: str, port: int, fault: dict[str, int | str]) -> None:
    payload = json.dumps(fault).encode("utf-8")
    with socket.create_connection((host, port), timeout=1.0) as connection:
        connection.sendall(payload)


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare virtual charger controller processes")
    parser.add_argument("--count", type=int, default=2)
    parser.add_argument("--start-port", type=int, default=9000)
    args = parser.parse_args()
    commands = build_controller_commands(args.count, "127.0.0.1", args.start_port)
    print(json.dumps(commands, indent=2))


if __name__ == "__main__":
    main()
