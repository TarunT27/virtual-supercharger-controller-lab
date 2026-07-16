import json
import socket
import subprocess
import sys
import threading
import time
import unittest
from dataclasses import FrozenInstanceError
from io import StringIO
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from orchestrator import (
    ControllerEndpoint,
    ProtocolError,
    build_controller_commands,
    inject_tcp_fault,
    launch_controllers,
    main,
    run_demo,
    send_fault,
    send_request,
    terminate_controllers,
    wait_until_ready,
)


class OneShotServer:
    def __init__(self, response: bytes):
        self.response = response
        self.received = b""
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(1)
        self.port = self.socket.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self):
        connection, _ = self.socket.accept()
        with connection:
            while not self.received.endswith(b"\n"):
                chunk = connection.recv(4096)
                if not chunk:
                    break
                self.received += chunk
            connection.sendall(self.response)
        self.socket.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.thread.join(timeout=2)


class OrchestratorTests(unittest.TestCase):
    def test_controller_endpoint_is_validated_and_immutable(self):
        endpoint = ControllerEndpoint("127.0.0.1", 9000, "charger-0")

        with self.assertRaises(FrozenInstanceError):
            endpoint.port = 9001
        for values in [
            ("", 9000, "charger-0"),
            ("127.0.0.1", 0, "charger-0"),
            ("127.0.0.1", 9000, ""),
        ]:
            with self.subTest(values=values), self.assertRaises(ValueError):
                ControllerEndpoint(*values)

    def test_builds_validated_immutable_commands(self):
        commands = build_controller_commands(
            count=3,
            host="127.0.0.1",
            start_port=9000,
            executable="build/controller_lab",
            maximum_power_kw=180.0,
        )

        self.assertIsInstance(commands, tuple)
        self.assertEqual(3, len(commands))
        self.assertEqual("charger-0", commands[0][commands[0].index("--id") + 1])
        self.assertEqual("9002", commands[2][commands[2].index("--port") + 1])
        self.assertIn("180.0", commands[0])

    def test_rejects_invalid_fleet_configuration(self):
        invalid = [
            {"count": 0, "start_port": 9000},
            {"count": 101, "start_port": 9000},
            {"count": 2, "start_port": 65535},
            {"count": 1, "start_port": 0},
            {"count": 1, "start_port": 9000, "host": "example.com"},
            {"count": 1, "start_port": 9000, "host": "0.0.0.0"},  # noqa: S104
            {"count": 1, "start_port": 9000, "maximum_power_kw": 1_001.0},
        ]
        for values in invalid:
            with self.subTest(values=values), self.assertRaises(ValueError):
                build_controller_commands(**{"host": "127.0.0.1", **values})

    def test_builds_bounded_fault_messages(self):
        self.assertEqual(
            {"command": "inject_fault", "kind": "delay", "duration_ms": 250},
            inject_tcp_fault(kind="delay", duration_ms=250),
        )
        for kind, duration in [
            ("unknown", 0),
            ("delay", -1),
            ("delay", 30_001),
            ("disconnect", 1),
        ]:
            with (
                self.subTest(kind=kind, duration=duration),
                self.assertRaises(ValueError),
            ):
                inject_tcp_fault(kind=kind, duration_ms=duration)

    def test_send_request_frames_and_validates_response(self):
        response = {
            "version": 1,
            "request_id": "request-1",
            "success": True,
            "controller_id": "charger-0",
            "data": {"state": "idle"},
            "error": None,
        }
        with OneShotServer((json.dumps(response) + "\n").encode()) as server:
            result = send_request(
                ControllerEndpoint("127.0.0.1", server.port, "charger-0"),
                {"command": "status"},
                request_id="request-1",
            )

        self.assertEqual(response, result)
        request = json.loads(server.received)
        self.assertEqual(1, request["version"])
        self.assertEqual("request-1", request["request_id"])
        self.assertEqual("status", request["command"])

    def test_send_request_rejects_corrupt_response(self):
        with OneShotServer(b"{corrupt\n") as server, self.assertRaises(ProtocolError):
            send_request(
                ControllerEndpoint("127.0.0.1", server.port, "charger-0"),
                {"command": "status"},
                request_id="request-2",
            )

    def test_send_request_rejects_mismatched_response_envelope(self):
        response = {
            "version": 1,
            "request_id": "wrong-request",
            "success": True,
            "controller_id": "charger-0",
            "data": {},
            "error": None,
        }
        with (
            OneShotServer((json.dumps(response) + "\n").encode()) as server,
            self.assertRaises(ProtocolError),
        ):
            send_request(
                ControllerEndpoint("127.0.0.1", server.port, "charger-0"),
                {"command": "status"},
                request_id="request-3",
            )

    def test_send_request_rejects_non_json_request_values(self):
        endpoint = ControllerEndpoint("127.0.0.1", 9000, "charger-0")
        with self.assertRaises(ValueError):
            send_request(endpoint, {"command": "status", "invalid": object()})
        with self.assertRaises(ValueError):
            send_request(endpoint, {"command": "status"}, request_id="")

    def test_send_fault_uses_protocol_client(self):
        endpoint = ControllerEndpoint("127.0.0.1", 9000, "charger-0")
        fault = inject_tcp_fault("disconnect")
        with mock.patch("orchestrator.send_request", return_value={"success": True}) as request:
            result = send_fault(endpoint, fault, request_id="fault-1")
        self.assertEqual({"success": True}, result)
        request.assert_called_once_with(endpoint, fault, request_id="fault-1")

    def test_wait_until_ready_retries_until_health_succeeds(self):
        endpoint = ControllerEndpoint("127.0.0.1", 9000, "charger-0")
        with mock.patch(
            "orchestrator.send_request",
            side_effect=[OSError("not ready"), {"success": True}],
        ) as request:
            wait_until_ready(endpoint, timeout_seconds=0.5, poll_interval_seconds=0.001)
        self.assertEqual(2, request.call_count)

    def test_wait_until_ready_has_a_bounded_timeout(self):
        endpoint = ControllerEndpoint("127.0.0.1", 9000, "charger-0")
        with (
            mock.patch("orchestrator.send_request", side_effect=OSError("not ready")),
            self.assertRaises(TimeoutError),
        ):
            wait_until_ready(
                endpoint,
                timeout_seconds=0.002,
                poll_interval_seconds=0.001,
            )

    def test_launch_controllers_rolls_back_after_partial_failure(self):
        process = mock.Mock()
        with (
            mock.patch(
                "orchestrator.subprocess.Popen",
                side_effect=[process, OSError("launch failed")],
            ),
            mock.patch("orchestrator.terminate_controllers") as terminate,
            self.assertRaises(OSError),
        ):
            launch_controllers((("controller",), ("controller",)))
        terminate.assert_called_once_with((process,))

    def test_terminate_controllers_escalates_stuck_process(self):
        process = mock.Mock()
        process.poll.side_effect = [None, None, 1]
        process.wait.side_effect = [subprocess.TimeoutExpired("controller", 0.01)]

        terminate_controllers((process,), timeout_seconds=0.01)

        process.terminate.assert_called_once_with()
        process.kill.assert_called_once_with()

    def test_terminate_controllers_continues_after_one_child_fails(self):
        failed_process = mock.Mock()
        failed_process.poll.return_value = None
        failed_process.terminate.side_effect = OSError("access denied")
        failed_process.wait.side_effect = subprocess.TimeoutExpired("controller", 0.01)
        healthy_process = mock.Mock()
        healthy_process.poll.side_effect = [None, 0, 0]

        terminate_controllers((failed_process, healthy_process), timeout_seconds=0.01)

        failed_process.terminate.assert_called_once_with()
        failed_process.kill.assert_called_once_with()
        healthy_process.terminate.assert_called_once_with()

    def test_run_demo_exercises_all_faults_and_cleans_up(self):
        processes = (mock.Mock(), mock.Mock(), mock.Mock())
        for process in processes:
            process.poll.return_value = None

        def request_response(_endpoint, request, **_kwargs):
            command = request["command"]
            data = {
                "start_session": {"state": "charging"},
                "allocate_power": {"allocated_power_kw": request.get("requested_power_kw")},
                "recover": {"state": "idle"},
                "stop_session": {"state": "idle"},
                "shutdown": {"state": "idle"},
            }[command]
            return {"success": True, "data": data}

        def fault_response(_endpoint, fault, **_kwargs):
            if fault["kind"] in {"disconnect", "corrupt"}:
                raise ProtocolError("expected transport fault")
            time.sleep(fault["duration_ms"] / 1000.0)
            return {"success": True, "data": {"state": "faulted"}}

        with (
            mock.patch("orchestrator.launch_controllers", return_value=processes),
            mock.patch("orchestrator.wait_until_ready"),
            mock.patch("orchestrator.send_request", side_effect=request_response),
            mock.patch("orchestrator.send_fault", side_effect=fault_response),
            mock.patch("orchestrator.terminate_controllers") as terminate,
        ):
            report = run_demo(count=3, executable="controller", start_port=19000)

        self.assertEqual(3, report["controllers_started"])
        self.assertEqual(
            {"delay", "disconnect", "corrupt"},
            {item["fault_kind"] for item in report["controllers"]},
        )
        self.assertTrue(all(item["recovered_state"] == "idle" for item in report["controllers"]))
        self.assertTrue(all(item["session_stopped"] for item in report["controllers"]))
        self.assertTrue(all(item["fault_observed"] for item in report["controllers"]))
        for process in processes:
            process.wait.assert_called_once_with(timeout=0.5)
        terminate.assert_called_once_with(processes)

    def test_run_demo_rejects_health_from_an_unowned_endpoint(self):
        exited_process = mock.Mock()
        exited_process.poll.return_value = 3
        with (
            mock.patch("orchestrator.launch_controllers", return_value=(exited_process,)),
            mock.patch("orchestrator.wait_until_ready"),
            mock.patch("orchestrator.terminate_controllers") as terminate,
            self.assertRaises(RuntimeError),
        ):
            run_demo(count=1, executable="controller")
        terminate.assert_called_once_with((exited_process,))

    def test_run_demo_cleans_up_when_readiness_fails(self):
        processes = (mock.Mock(),)
        with (
            mock.patch("orchestrator.launch_controllers", return_value=processes),
            mock.patch("orchestrator.wait_until_ready", side_effect=TimeoutError("not ready")),
            mock.patch("orchestrator.terminate_controllers") as terminate,
            self.assertRaises(TimeoutError),
        ):
            run_demo(count=1, executable="controller")
        terminate.assert_called_once_with(processes)

    def test_main_supports_dry_run_and_demo_output(self):
        output = StringIO()
        with mock.patch("sys.stdout", output):
            self.assertEqual(0, main(["--count", "1", "--dry-run"]))
        self.assertIn("charger-0", output.getvalue())

        output = StringIO()
        with (
            mock.patch("orchestrator.run_demo", return_value={"controllers_started": 1}),
            mock.patch("sys.stdout", output),
        ):
            self.assertEqual(0, main(["--count", "1"]))
        self.assertEqual(1, json.loads(output.getvalue())["controllers_started"])


if __name__ == "__main__":
    unittest.main()
