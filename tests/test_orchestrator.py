import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from orchestrator import build_controller_commands, inject_tcp_fault


class OrchestratorTests(unittest.TestCase):
    def test_builds_one_process_command_per_controller(self):
        commands = build_controller_commands(count=3, host="127.0.0.1", start_port=9000)

        self.assertEqual(3, len(commands))
        self.assertEqual("charger-0", commands[0][-2])
        self.assertEqual("9002", commands[2][-1])

    def test_builds_fault_message_without_mutating_inputs(self):
        fault = inject_tcp_fault(kind="delay", duration_ms=250)

        self.assertEqual({"type": "delay", "duration_ms": 250}, fault)

    def test_rejects_invalid_process_count(self):
        with self.assertRaises(ValueError):
            build_controller_commands(count=0, host="127.0.0.1", start_port=9000)


if __name__ == "__main__":
    unittest.main()
