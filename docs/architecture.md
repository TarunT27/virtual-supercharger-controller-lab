# Architecture and engineering decisions

## System boundary

The lab intentionally models the controller boundary, not the full charging ecosystem. Each C++ process owns one controller state machine and one TCP listener. Python owns fleet creation, readiness, scenario sequencing, fault observation, and teardown.

This split makes failures realistic enough for system-in-the-loop testing while keeping the domain model independently testable.

## Components

- `Controller` is the synchronized domain core. Configuration is immutable after construction; every snapshot is returned as a new value.
- `protocol.cpp` validates versioned JSON and maps allow-listed commands to domain transitions. It never performs socket I/O.
- `server.cpp` owns the network-message worker, bounded framing, socket timeouts, connection closure, and deliberate transport corruption. Separate state-observer and telemetry workers emit real snapshots from the synchronized domain core.
- `orchestrator.py` uses frozen endpoint values, validates fleet configuration, launches without a shell, waits on health rather than arbitrary sleeps, and always tears down children in `finally`.

## Safety invariants

1. Power is allocated only while charging.
2. Allocated power never exceeds the controller's immutable maximum.
3. Invalid, non-finite, zero, or negative power requests never energize the controller.
4. Any injected network fault clears allocated power before the transport effect is visible.
5. Recovery returns to idle and never silently resumes a previous vehicle session.
6. The default listener is loopback-only.
7. Frames, delays, controller counts, ports, and timeouts are bounded.

## Fault semantics

Faults live at the transport adapter boundary but update the domain first. That ordering makes the fail-safe state deterministic even when a disconnect or corrupt response prevents the client from seeing an acknowledgment.

The Python demo treats protocol failure as the expected observation for corruption, reconnects, sends `recover`, and verifies the returned state. Faults are isolated because every controller is a separate process with a distinct port.

## Process lifecycle

Fleet startup is transactional: if any process fails to launch or become healthy, every process that did start is terminated. Normal teardown first sends a protocol shutdown, then uses bounded terminate/kill fallbacks. Standard streams are redirected to prevent pipe deadlocks during unattended scenarios.

## Deliberate limitations

- No OCPP, PLC, CAN, ISO 15118, real metering, or hardware timing model
- No TLS or authentication; remote exposure is out of scope
- One controller per process and a sequential connection handler
- Fault injection is deterministic and synthetic, intended for repeatable tests

These constraints keep the lab auditable and portable. A future hardware-in-the-loop adapter can reuse the controller and protocol tests without changing the fleet control plane.
