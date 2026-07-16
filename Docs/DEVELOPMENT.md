# Development

## Prerequisites

- Visual Studio 2026 Community or Build Tools
- Desktop development with C++ workload
- .NET desktop development workload
- CMake 4.2 or newer
- Windows SDK
- .NET 10 SDK (selected by `global.json`)
- PowerShell 7 or Windows PowerShell 5.1

The native build is x64-only. Required Steinberg ASIO interface files are
vendored under `third_party/asio`, so a separate SDK installation is not
needed. Physical testing requires the hardware vendor's 64-bit ASIO driver.

The WPF SDK is part of the Windows .NET SDK; no separate `dotnet workload`
installation is required. The desktop development workload supplies the Visual
Studio WPF tooling.

## Configure, build, and test

```powershell
.\build.ps1 -Configuration Debug -Test
```

`build.ps1` uses CMake from `PATH` when available and otherwise locates the
Visual Studio bundled CMake through `vswhere`. The script resolves presets,
`global.json`, projects, and outputs relative to the repository, so it can also
be invoked by absolute path from another current directory. It always builds in
this order:

1. configure and build the x64 native C++ targets;
2. run the native CTest suite when `-Test` is present;
3. restore and build the .NET 10 WPF application;
4. build and run the dependency-free managed test executable when `-Test` is present.

The managed tests primarily use an in-process Fake worker and also exercise the
native JSON worker contract through `fake:loopback`; they do not open an ASIO
driver. They also launch a controlled helper process to verify worker
serialization, Job Object cancellation bounds, exit-code/result invariants,
warning propagation, and malformed-result rejection.

Release validation:

```powershell
.\build.ps1 -Configuration Release -Test
```

Clean rebuild:

```powershell
.\build.ps1 -Configuration Debug -Test -Clean
```

`-Clean` removes only the known native, managed, and publish output directories
under `out/` after verifying that every path remains inside the repository.

## Run locally

The WPF build output is configuration-specific:

```powershell
$app = '.\out\dotnet\CapturePanel.App\Debug\net10.0-windows\win-x64\CapturePanel.exe'
& $app
```

`capture-panel.exe` must remain beside `CapturePanel.exe`; the WPF application
starts one short-lived worker process for device discovery, setup tests, and
captures. To expose `fake:loopback` during UI development:

```powershell
$env:CAPTURE_PANEL_SHOW_FAKE = '1'
& $app
```

The native CLI remains independently usable from
`out\build\windows-x64\bin\<Configuration>\capture-panel.exe`.

## Windows icon assets

`assets/windows/CapturePanel.svg` is the editable master,
`CapturePanel.ico` is the checked-in Windows Shell asset, and
`CapturePanel.png` is its 256-pixel in-app representation. The SVG is authored
directly on a tightly fitted 256-pixel canvas and rendered with an antialiased
SVG converter. The approved artwork uses a rounded-square background and
upper-left-to-lower-right 45-degree gradients; alternate icon variants and
generated previews are not kept in the repository. The ICO contains one
PNG-compressed 256-pixel 32-bit frame and lets Windows scale it for the
requested DPI. It is embedded in both executables; WPF leaves `Window.Icon`
unset so the Shell owns scaling.

## Publish layout

The release workflow performs a self-contained, untrimmed `win-x64` WPF folder
publish. The binary ZIP has `CapturePanel.exe` and `capture-panel.exe` in the
same directory, followed by the .NET runtime files, `LICENSE`,
`THIRD_PARTY_NOTICES.md`, the Steinberg SDK license, and project documentation.
The package also preserves the .NET and Windows Desktop Runtime licenses and
third-party notices selected by the self-contained publish. The corresponding
tagged source ZIP and `SHA256SUMS` are separate release assets. Hardware vendor
drivers are never copied into build or publish output.

## Development rules

- Put platform-independent behavior in `capture_panel_core`.
- Add Windows/ASIO code only under `src/backends/asio`.
- Keep the Fake backend behavior deterministic.
- Keep arbitrary-duration capture paths chunked; whole-file `AudioBuffer`
  materialization is reserved for bounded test and setup-verification fixtures.
- Keep the WPF/native boundary on the versioned JSON Lines worker protocol.
- Keep `capture-panel.exe` adjacent to `CapturePanel.exe` in build and publish output.
- Do not allocate, lock, log, perform file I/O, or call managed code from an ASIO callback.
- Add or port a test before changing alignment and verification constants.
- Public channel numbers are one-based; backend indices are zero-based.
- Core audio buffers are normalized interleaved float32.
- Do not commit `out/`, generated Visual Studio files, or `CMakeUserPresets.json`.

## Test boundaries

The normal CTest suite does not open an ASIO driver. It validates the core,
streaming WAV/Float32 assets, SPSC ring and stream workers, Fake backend, ASIO
sample conversion, buffer timeline, device-ID parsing, backend routing, CLI,
and JSON worker output. The managed test executable
validates WPF view-model state, settings, WAV metadata, capture-file promotion,
and the native JSON worker contract with `fake:loopback`. Native driver
callbacks and physical routing use the protected manual procedure in
[HARDWARE_TESTING.md](HARDWARE_TESTING.md).
