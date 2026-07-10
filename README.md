# Capture Panel for Windows

Capture Panel for Windows plays a WAV source through selected hardware output
channels, records the external return, aligns it to the source, and verifies the
round-trip route.

The repository contains the native C++20 core and CLI, deterministic Fake
backend, native Windows ASIO backend, and a .NET 10 WPF desktop application.
The desktop application runs `capture-panel.exe` as an isolated, versioned JSON
worker so a faulty vendor ASIO driver cannot run inside the WPF process.

## Implemented

- PCM WAV input and output (16/24/32-bit plus float32 input)
- marker-based latency detection, alignment, and setup verification
- one-based channel parsing and validation
- capture progress, cancellation, timeout, and driver-event handling
- ASIO driver discovery from the 64-bit Windows registry
- ASIO PCM conversion for integer and floating-point LSB/MSB formats
- allocation-free ASIO double-buffer callback processing
- `devices`, `channels`, `test`, and `run` CLI commands
- Fake full-duplex loopback for deterministic development and CI
- WPF source, route, setup-test, capture, progress, cancellation, and settings workflow
- managed view-model, settings, WAV-metadata, and output-promotion tests

## Build and test

Requirements are Visual Studio 2026 with the Desktop development with C++ and
.NET desktop development workloads, CMake 4.2 or newer, a Windows SDK, and the
.NET 10 SDK. The checked-in `global.json` selects the supported SDK feature
band.

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
```

The script builds the native C++ targets first, runs CTest when requested, then
builds the WPF application and its managed deterministic tests. Main outputs
are:

- WPF: `out\dotnet\CapturePanel.App\<Configuration>\net10.0-windows\win-x64\CapturePanel.exe`
- bundled worker/CLI: the adjacent `capture-panel.exe`
- standalone CLI: `out\build\windows-x64\bin\<Configuration>\capture-panel.exe`

Launch the Debug desktop application:

```powershell
& '.\out\dotnet\CapturePanel.App\Debug\net10.0-windows\win-x64\CapturePanel.exe'
```

For development without hardware, set `CAPTURE_PANEL_SHOW_FAKE=1` before
launching to make the Fake device visible in the WPF application.

List every registered ASIO driver plus the Fake device:

```powershell
.\out\build\windows-x64\bin\Debug\capture-panel.exe devices
```

Driver IDs are stable registry identities such as
`asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`. Public channel numbers are
one-based, matching the labels shown by the CLI; the backend performs the ASIO
zero-based translation.

A deterministic smoke test needs no hardware:

```powershell
$cli = '.\out\build\windows-x64\bin\Debug\capture-panel.exe'
& $cli test --driver fake:loopback --play-channel 1 --record-channel 1 --verbose
```

For physical ASIO testing, install the device vendor's 64-bit driver first and
follow [the hardware test guide](Docs/HARDWARE_TESTING.md). The known Anagram
loopback route is physical output 9 to physical input 1.

## Release automation

A pushed `vX.Y.Z` tag matching both application versions triggers the Release
workflow. It rebuilds and tests native and managed code, publishes the WPF app
as a self-contained `win-x64` deployment, places `capture-panel.exe` beside it,
and creates a combined ZIP. The release also contains GPL/third-party licenses,
the .NET runtime license/notices, an exact tagged source archive, and SHA-256
sums. It never packages a hardware vendor driver, and the desktop ZIP does not
require a separately installed .NET runtime or Visual C++ Redistributable.

## Documentation

- [ASIO backend](Docs/ASIO_BACKEND.md)
- [Hardware testing](Docs/HARDWARE_TESTING.md)
- [Port status](Docs/PORT_STATUS.md)
- [Windows architecture](Docs/WINDOWS_ARCHITECTURE.md)
- [Windows UI design](Docs/WINDOWS_UI_DESIGN.md)
- [Development](Docs/DEVELOPMENT.md)
- [macOS analysis and Windows port plan](Docs/MACOS_ANALYSIS_AND_WINDOWS_PORT_PLAN.md)

## License

Copyright (C) 2026 Lee Hui-Jong.

Capture Panel for Windows is free software licensed under
[GNU GPL v3.0 only](LICENSE). The Steinberg ASIO SDK interface files are used
under their GPLv3 alternative; preserved notices and details are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Hardware vendor drivers are
not distributed by this project.
