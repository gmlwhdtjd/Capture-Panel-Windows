# Core and CLI Port Status

## Scope

This status tracks the Windows port of the platform-independent behavior in
`Capture-Panel-Mac` at commit `8499e22eb7689748c610075606606790114c1e34`.
The WPF UI and the physical ASIO transport are intentionally outside this
milestone.

## Implemented

| Area | Windows implementation | Automated coverage |
|---|---|---|
| Audio model | normalized interleaved float32, peak/RMS, dB gain, frame extraction | unit tests |
| WAV | RIFF parsing, PCM 16/24/32, float32, extensible format, PCM writing | fixture and round-trip tests |
| Channels | one-based lists and ranges, device-bound validation | parser and failure tests |
| Playback plan | five markers, marker spacing, marker-to-payload silence | algorithm tests |
| Alignment | adaptive marker detection, sequence fitting, latency, trim, zero padding | ported edge-case tests |
| Setup verification | log sweep, correlation, timing, ambiguity, clipping, missing signal, decay | ported edge-case tests |
| Capture orchestration | validation, sample-rate restore, progress, cancellation, events, aligned output | Fake end-to-end tests |
| Backend boundary | device provider and raw full-duplex capture interfaces | Fake contract tests |
| Fake backend | deterministic 8x8 loopback, latency, gain, padding, progress, cancellation | unit and end-to-end tests |
| CLI | `devices`, `channels`, `test`, `run`, help, version, license | parser, diagnostics, UTF-8 path, end-to-end tests |

The test executable currently contains 55 deterministic tests and does not
require an audio driver or physical device.

## Intentional Windows differences

- ASIO exposes inputs and outputs through one selected driver, so Windows uses
  one string `--driver` instead of separate Core Audio playback and recording
  device IDs.
- The first Windows release does not combine two unrelated ASIO drivers.
- macOS aggregate-device creation, `--clock-source`, and drift-compensation
  selection are Core Audio-specific and are not part of the Windows contract.
- Public channel numbers remain one-based. A backend translates them to its
  native zero-based representation.

## Deferred to the ASIO milestone

- driver discovery, load/unload, and driver-specific diagnostics
- ASIO buffer-size and sample-type negotiation
- native ASIO sample conversion
- real-time double-buffer callbacks and preallocated transfer buffers
- timeout, reset, resync, and device-disconnect behavior
- measured latency and physical loopback validation
- ASIO SDK source/notice packaging and `THIRD_PARTY_NOTICES.md`

The core and CLI do not need to change shape for this work. The executable will
replace the injected `FakeAudioBackend` with an ASIO implementation of the same
two interfaces.

## Fake CLI smoke flow

```powershell
capture-panel devices
capture-panel channels fake:loopback
capture-panel test `
  --driver fake:loopback `
  --play-channel 1 `
  --record-channel 1 `
  --verbose
capture-panel run `
  --input source.wav `
  --output recorded.wav `
  --driver fake:loopback `
  --play-channel 1 `
  --record-channel 1
```

## Verification record

- Date: 2026-07-10
- Visual Studio Community 2026 18.7.3
- MSVC 19.51.36248 / v145 toolset
- CMake 4.3.1 and Windows SDK 10.0.26100.0
- Debug: build passed; 55/55 tests passed
- Release: build passed; 55/55 tests passed
- CLI smoke: default device list, channel list, verbose setup verification,
  version, and license commands passed on `fake:loopback`

Hosted CI is defined in `.github/workflows/ci.yml` and repeats Debug and Release
validation on `windows-2025-vs2026`. Physical ASIO validation remains a manual or
self-hosted hardware job.

## License

Capture Panel for Windows is released under GPL-3.0-only. The repository
contains the official license text in `LICENSE`. Binary release automation must
ship that file, the exact corresponding source, build scripts, and all required
ASIO notices and source once the ASIO backend is added.
