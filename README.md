# Capture Panel for Windows

Capture Panel for Windows plays a WAV source through selected output channels,
records the external hardware return, verifies the route, and writes an aligned
WAV capture.

This repository currently contains the platform-independent C++20 core, the
native CLI, and a deterministic Fake full-duplex backend. The ASIO backend and
WPF application are separate later milestones.

## Current scope

- WAV PCM input/output
- channel parsing and validation
- marker-based round-trip alignment
- setup verification signal and timing analysis
- capture progress, cancellation, warnings, and failures
- `devices`, `channels`, `test`, and `run` CLI commands
- Fake loopback audio device for development and CI

See [port status](Docs/PORT_STATUS.md), [Windows architecture](Docs/WINDOWS_ARCHITECTURE.md),
[development](Docs/DEVELOPMENT.md), and the
[macOS analysis and port plan](Docs/MACOS_ANALYSIS_AND_WINDOWS_PORT_PLAN.md).

## License

Copyright (C) 2026 Lee Hui-Jong. Capture Panel for Windows is free software
licensed under [GNU GPL v3.0 only](LICENSE).
