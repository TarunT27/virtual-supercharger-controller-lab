# Virtual Supercharger Controller Lab

Starter system-in-the-loop prototype for experimenting with virtual EV charging controllers.

## Included now

- Thread-safe C++ controller state and power-allocation model
- Three demonstration workers for controller state, network messages, and telemetry
- Python helpers for preparing multiple controller processes
- TCP fault messages for delay, disconnect, and corruption scenarios
- C++ and Python tests plus a minimal Docker build

This is an initial prototype. The next iteration will connect the C++ worker to real TCP messages and expand the fault-recovery scenarios.

## Run

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
python -m unittest discover -s tests -p "test_*.py"
python python/orchestrator.py --count 3
```
