# Security policy

## Scope

This repository is a local test lab, not production charging infrastructure. The daemon accepts only the IPv4 loopback address `127.0.0.1`; remote binding is not supported.

## Reporting

Report suspected vulnerabilities privately through GitHub's repository security advisory flow. Do not open a public issue containing exploit details.

## Defensive behavior

- Requests and responses are bounded to 65,536 bytes.
- Socket reads and orchestration waits have deadlines.
- Fault delays are capped at 30 seconds.
- Fleet size is capped at 100 and port ranges are validated.
- Child processes launch without a shell and are cleaned up on partial failure.
- Invalid state transitions fail closed and faults drop allocated power to zero.

Do not expose the listener to an untrusted network or use the simulator to control real charging equipment.
