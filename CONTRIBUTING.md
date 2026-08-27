# Contributing to Decode Fabric

Decode Fabric accepts contributions from individuals and organizations on the
terms of the Apache License 2.0 without requiring a Contributor License
Agreement (CLA).

## Process

1. Fork the repository and create a feature branch.
2. Build and test (see README).
3. Keep the compiler diagnostics strict: the project builds with /W4 /WX (MSVC)
   or -Wall -Wextra -Werror elsewhere. Any warning is a build failure.
4. Add tests for any behavioral change under tests/ (they register automatically
   through CTest).
5. Run the full test suite before submitting: ctest --test-dir build.
6. Open a pull request describing the change and the evidence (test output).

## Style

- C++20, no exceptions for ordinary control flow (use Result<T> / ErrorCode).
- Strongly typed ids for identities; never truncate or reinterpret 64-bit ids.
- Thread-safety: never invoke a DecodeExecutor, perform network/blocking I/O, or
  re-enter a lock while holding a state lock (see docs/architecture.md).
- Document thread-safety guarantees and runtime semantics; keep measured,
  derived, configured, and estimated values clearly distinguished.

## Telemetry

Decode Fabric does not collect or transmit telemetry. Do not add telemetry that
leaves the machine.
