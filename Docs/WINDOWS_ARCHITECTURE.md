# Capture Panel Windows Architecture

## Decisions

| Area | Decision |
|---|---|
| Audio domain and real-time code | C++20 |
| Native build | CMake 4.2+ and checked-in presets |
| Compiler | Visual Studio 2026 / MSVC v145, x64 |
| Native tests | CTest with a dependency-free test harness |
| Windows audio | native ASIO backend behind core interfaces |
| Offline development/CI | deterministic Fake full-duplex backend |
| CLI | native C++ executable linked directly to the core |
| GUI | C# WPF on .NET 10, x64 |
| GUI boundary | short-lived native CLI worker using versioned JSON Lines |
| Local orchestration | `build.ps1`; mise is not required |
| CI | hosted Windows builds; manual/protected physical audio tests |
| License | GPL-3.0-only with corresponding source and notices |

## Target graph

```text
capture_panel_core
  |-- streaming WAV readers/writer, levels, capture orchestration
  |-- bounded-window marker alignment and setup verification
  |-- private Windows atomic-file transaction adapter
  +-- IAudioDeviceProvider + IAudioCaptureBackend
          |
          +-- FakeAudioBackend
          +-- AsioAudioBackend
                    |-- 64-bit registry discovery
                    |-- COM driver session + hidden HWND
                    |-- PCM sample conversion
                    |-- bounded playback/record SPSC rings
                    +-- allocation-free double-buffer callback

BackendRouter
  +-- combines Fake and ASIO identities for the CLI

capture-panel.exe
  +-- core + BackendRouter

CapturePanel.exe (WPF)
  +-- capture-panel.exe (one isolated worker process per operation)
          +-- JSON protocol + CLI command layer
          +-- core + BackendRouter + ASIO backend
```

The core public API and audio algorithms never expose Windows, ASIO, WPF, or
.NET types. A private Windows adapter performs same-directory atomic output
promotion without leaking Win32 handles into the WAV layer. Core audio is
normalized interleaved float32. The ASIO boundary converts it to and from the
native channel-planar driver format.

## Repository layout

```text
Capture-Panel-Windows/
|-- CMakeLists.txt
|-- CMakePresets.json
|-- build.ps1
|-- Docs/
|-- src/
|   |-- core/
|   |-- backends/
|   |   |-- fake/
|   |   |-- asio/
|   |   +-- router/
|   |-- cli/
|   |-- platform/windows/
|   +-- ui/CapturePanel.App/
|-- tests/
|   +-- ui/
|-- third_party/asio/
|-- LICENSE
+-- THIRD_PARTY_NOTICES.md
```

## Backend contract

`IAudioDeviceProvider` lists devices/channels and changes sample rate.
`IAudioCaptureBackend` performs a synchronous full-duplex capture from a
`CapturePassPlaybackPlan`, which describes a source reader, marker positions,
gain, and logical frame boundaries without containing the complete source.
The result is a `Float32AudioAsset` (normally a temporary interleaved file) and
the number of pre-padding frames.

ASIO driver IDs use `asio:{CANONICAL-CLSID}`. Fake uses `fake:loopback`.
No ASIO buffer, sample type, COM object, or clock type crosses the core
boundary.

One ASIO driver supplies both input and output for a pass. The first release
does not combine unrelated drivers, open a driver control panel, or silently
restart a pass after reset/resync/rate-change/overload events.

## ASIO threading and lifetime

A capture owns one STA COM apartment, one hidden top-level host window, and one
`IASIO` instance on the same control thread. Lifecycle is:

```text
CoInitializeEx(STA) -> hidden HWND -> CoCreateInstance -> init
  -> rate/channel/buffer negotiation -> createBuffers -> start
  -> control-thread wait/message pump
  -> stop -> disposeBuffers -> Release -> DestroyWindow -> CoUninitialize
```

The driver callback performs only bounded sample copies/conversion, exact SPSC
ring transfers, and atomic state publication. It does not allocate, lock, log,
perform file I/O, invoke progress handlers, or touch managed code. Source
decoding and raw recording writes run on separate workers. Progress,
cancellation, timeouts, and driver event decisions run on the control thread.

Because ASIO callbacks carry no user-data pointer, the process permits one
active ASIO operation. A process-wide lease serializes discovery, channel
queries, sample-rate changes, and captures so two threads cannot initialize the
same vendor driver concurrently. Callback targets use a lock-free lifetime
gate; teardown is
`stop -> close/drain callback gate -> stop/join workers -> disposeBuffers`,
including partial `createBuffers` and `start` failures. Public channel numbers
are one-based while `ASIOBufferInfo.channelNum` is zero-based. See
[Streaming Audio Architecture](STREAMING_AUDIO_ARCHITECTURE.md) for the complete
data flow.

## WPF boundary

The managed layer is control-plane only. It starts `capture-panel.exe` with
`--json`, reads one UTF-8 `capture-panel/1` JSON object per stdout line, and
terminates the worker to cancel or contain an unresponsive operation. Device
metadata, progress, diagnostics, and final results cross the process boundary;
audio samples and driver callbacks remain in the native worker.

Only one worker runs per application client. Every worker is assigned to a
`KILL_ON_JOB_CLOSE` Windows Job Object, and cancellation/error cleanup has a
bounded wait. The managed boundary treats JSON as untrusted protocol data: it
checks result invariants and exit codes, fingerprints the source before and
after work, validates the completed WAV against the request, and only then
promotes the temporary file to the selected destination.

The Test result's `inputPeakDbfs` is the peak after input trim, matching the
level shown by the desktop application. `digital_clipping` is evaluated against
both the raw recorded peak and the adjusted peak; consumers must use that
diagnostic rather than infer clipping from `inputPeakDbfs` alone.

ASIO drivers are in-process COM servers. Keeping each operation in a short-lived
worker prevents a vendor driver access violation from taking down the WPF
process and preserves the ASIO STA/hidden-window lifecycle already used by the
CLI. `capture-panel.exe` is therefore a required application file, not an
optional developer tool.

## CI and release

Hosted CI builds Debug and Release native targets first, then the .NET 10 WPF
application and managed tests. Release publishing creates one self-contained
`CapturePanel.exe`, then separates the native worker, packaged documentation,
and legal notices into `bin`, `docs`, and `licenses`. CI asserts this exact ZIP
root shape and smoke-starts the packaged UI with the Fake backend. It does not
open an ASIO driver. Physical routing remains a manual or protected self-hosted
gate because hosted runners have neither the vendor driver nor the device.
Release packaging runs with read-only repository permission; only the final
artifact-verification/release job receives `contents: write`, and it rechecks
the tag target and packaged SHA-256 values before publishing.
