# Core, CLI, ASIO, and WPF Port Status

## Scope

This status tracks the Windows port of the platform-independent behavior in
`Capture-Panel-Mac` at commit `3b4537ef7abe21f641c4ada84712a4f28836353c`,
plus the native Windows ASIO transport and .NET 10 WPF application.

## Implemented

| Area | Windows implementation | Verification |
|---|---|---|
| Audio model | normalized interleaved float32, levels, gain, frame extraction | unit tests |
| WAV | PCM 16/24/32, float32/extensible input, PCM output | round-trip and fixture tests |
| Channels | one-based lists/ranges and device-bound validation | parser and failure tests |
| Alignment | five-marker plan, sequence fit, latency trim, zero padding | ported edge cases |
| Setup verification | log sweep, timing, ambiguity, pre/post-trim clipping, missing signal, decay | ported edge cases |
| Capture orchestration | rate restore, progress, cancellation, events, output | Fake end-to-end tests |
| Fake backend | deterministic 8x8 full-duplex loopback | unit and CLI tests |
| Backend router | combined Fake and ASIO discovery/delegation | unit tests |
| ASIO discovery | 64-bit registry, canonical CLSID IDs, availability probing | pure helper tests + local smoke |
| ASIO formats | Int16/24/32, Float32/64, Int32 16/18/20/24 containers, LSB/MSB | conversion tests |
| ASIO streaming | COM/hidden HWND lifecycle, selected buffers, RT callback, timeout/cancel/reset/resync/overload handling | buffer-timeline tests + three Anagram passes |
| CLI | `devices`, `channels`, `test`, `run`, help/version/license | parser and end-to-end tests |
| Worker protocol | versioned UTF-8 JSON Lines for devices/channels/test/run/events/errors | native CLI JSON tests + managed parser use |
| WPF desktop | source/route selection, setup gate, capture/cancel/progress, local settings, About/license window | dependency-free managed tests |
| Licensing | SDK 2.3.4 headers, SDK license, notices, GPL release requirements | repository audit |

The deterministic native and managed UI test executables require neither an
audio driver nor a physical device. Their current totals are reported by each
test runner during the build instead of being duplicated here.

## Windows-specific behavior

- A route uses one ASIO driver ID because ASIO exposes its inputs and outputs as
  one synchronous device.
- The first release does not combine unrelated ASIO drivers.
- Core Audio aggregate-device, clock-source, and drift-compensation controls do
  not map directly to this Windows contract.
- Public channels are one-based. ASIO `channelNum` is zero-based internally.
- Duplicate physical channels in one ASIO route are rejected because one driver
  buffer cannot represent two independent logical mappings safely.
- Setup Test reports `inputPeakDbfs` after input trim. Clipping evaluates the
  higher of the pre-trim and post-trim peaks, so attenuation cannot hide ADC/raw
  clipping and positive gain cannot introduce clipping unnoticed.

## Verification record

Date: 2026-07-11

- Visual Studio Community 2026 18.7.3/18.7.8 build tools
- MSVC 19.51 / v145 toolset
- CMake 4.3.1 and Windows SDK 10.0.26100.0
- Debug build: passed
- Debug deterministic tests: passed
- CLI registry smoke: Realtek ASIO probed and its 2 inputs/2 outputs listed
- CLI Fake setup verification: passed at -36 dBFS peak
- Darkglass ASIO discovery: 3 inputs, 9 outputs, 48 kHz
- Anagram output 9 -> input 1: three Debug and four Release passes at -36 dBFS
  (312-348 frames / 6.50-7.25 ms, timing error 0, warnings/failures 0)
- Release build and deterministic tests: passed
- WPF Debug/Release builds: passed
- Managed UI test suite: passed

Release validation is repeated before handoff. Hosted CI builds and tests the
same deterministic suite. It cannot validate a vendor driver, callback timing,
or a physical cable.

## Hardware gate

The target Anagram output 9 to input 1 path passed seven times using the official
Darkglass USB Audio 5.72.0 driver. Details and safe reproduction steps are in
[HARDWARE_TESTING.md](HARDWARE_TESTING.md). Disconnect/reset/overload injection
and long-duration soak testing remain separate robustness gates.

## Remaining product milestones

- add disconnect/reset/overload injection and long-duration hardware tests
- add full UI Automation, keyboard, screen-reader, and 200 percent scaling tests
- add installer/code-signing support after the portable ZIP release

## License release gate

Every binary release must include the GPLv3 license, third-party notices, the
Steinberg SDK license, and exact corresponding source/build scripts. Hardware
vendor ASIO drivers are never included.
