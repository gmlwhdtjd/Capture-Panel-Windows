# Capture Panel Windows Architecture

## Status

- Decision date: 2026-07-10
- Product license: GPL-3.0-only, with complete corresponding source distributed with releases
- Current milestone: platform-independent core and CLI running on a Fake audio backend
- Deferred milestone: Steinberg ASIO backend
- Deferred milestone: .NET 10 WPF application

## Technology decisions

| Area | Decision |
|---|---|
| Audio domain and real-time code | C++20 |
| Native build | CMake 4.2+ and `CMakePresets.json` |
| Native compiler | Visual Studio 2026 / MSVC v145 |
| Native tests | CTest with a dependency-free test harness during the core port |
| Windows audio | `IAudioCaptureBackend`, implemented by Fake now and ASIO later |
| CLI | Native C++ executable linked directly to the core |
| GUI | C# WPF on .NET 10 LTS after the native API stabilizes |
| GUI bridge | Thin C ABI DLL; no C++ ABI or audio buffer crosses into managed code |
| Local orchestration | `build.ps1`; mise is not required |
| CI | GitHub Actions `windows-2025-vs2026` hosted runner; self-hosted only for hardware tests |

## Target graph

```text
capture_panel_core (static C++ library)
  ├─ audio buffers, levels, WAV
  ├─ capture orchestration
  ├─ marker alignment
  └─ setup verification
          ↑
IAudioDeviceProvider + IAudioCaptureBackend
  ├─ FakeAudioBackend                current
  └─ AsioAudioBackend                deferred

capture-panel.exe ───────────────→ core + selected backend

CapturePanel.exe (WPF)             deferred
  → capture_panel_native.dll
    → core + ASIO backend
```

The `core` target cannot include Windows, ASIO, WPF, or .NET types. Audio inside the core is normalized interleaved float32.

## Repository layout

```text
Capture-Panel-Windows/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ build.ps1
├─ global.json                      reserved for the WPF milestone
├─ Docs/
├─ src/
│  ├─ core/
│  │  ├─ include/capture_panel/core/
│  │  └─ src/
│  ├─ backends/
│  │  ├─ fake/
│  │  └─ asio/                       deferred
│  ├─ cli/
│  ├─ native_api/                    deferred
│  └─ app/                           deferred WPF project
└─ tests/
```

## Backend contract

The core uses two narrow ports.

```text
IAudioDeviceProvider
  devices
  device
  channels
  set_sample_rate

IAudioCaptureBackend
  capture(RawAudioCaptureRequest) -> RawAudioCaptureResult
```

`RawAudioCaptureRequest` contains the selected route, prepared playback audio, pre/post padding, cancellation token, and progress callback. The result contains the full recorded float buffer and the number of pre-pad frames.

The Fake and ASIO implementations must obey the same observable contract. No ASIO-specific clock, buffer, driver, or sample type is visible above this boundary.

## Fake backend policy

The Fake backend is not a stub that merely returns success. It simulates a deterministic full-duplex loopback device so the complete product pipeline can run without hardware.

- One driver named `fake:loopback`
- Eight inputs and eight outputs
- Default sample rate 48 kHz
- Configurable round-trip latency and gain
- Pre-pad and post-pad recording
- Playback-to-record channel mapping
- Progress and cancellation
- Sample-rate changes exposed through the same device provider contract

The Fake backend must support these end-to-end operations:

```text
WAV input
→ validation
→ marker/payload playback plan
→ fake loopback capture
→ marker alignment
→ setup verification
→ aligned WAV output
```

This gives CI meaningful coverage before the ASIO implementation exists.

## WPF boundary

The future WPF application calls a native C ABI with opaque handles, UTF-8 strings, explicit memory ownership, and numeric error codes.

The managed layer may perform only control-plane operations:

- enumerate devices and channels
- update capture configuration
- start and cancel a pass
- poll progress and peak snapshots
- read results and error text

ASIO callbacks, playback samples, recorded samples, alignment, and verification remain entirely native. UI state is polled or dispatched at a non-real-time interval; a managed callback is never called from the ASIO real-time thread.

## Build and test

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
```

The underlying commands are:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Generated build trees live under `out/` and are not committed.

## CI and release

Hosted GitHub Actions jobs can build and test all source in the current milestone. They cannot prove physical routing or ASIO driver behavior.

`.github/workflows/ci.yml` runs the Fake-backed test suite in both Debug and
Release on every pull request and main-branch push.

```text
Pull request / main push
  → configure CMake
  → build Debug/Release
  → run core, Fake backend, and CLI tests

v* tag
  → Release build and tests
  → publish WPF app when available
  → bundle native DLLs, licenses, notices, and exact corresponding source
  → generate SHA-256
  → create GitHub Release

Manual hardware job
  → self-hosted Windows runner with the target ASIO device
```

If the ASIO SDK is stored as a Git submodule, release automation must create its own source bundle because GitHub's automatic source archive does not include submodule contents.

## License release gate

Before the first distributed binary:

- add the exact GPLv3 license variant used by the downloaded ASIO SDK
- preserve Steinberg notices and ASIO trademark guidance
- add `THIRD_PARTY_NOTICES.md`
- include complete source, build scripts, SDK source, and modifications for the exact binary
- expose license/source information in the CLI and later in the WPF settings page

No ASIO SDK source is added until its exact downloaded license text and version are recorded.
