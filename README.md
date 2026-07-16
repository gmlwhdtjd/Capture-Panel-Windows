# Capture Panel for Windows

Capture Panel for Windows is a desktop app and command-line tool for making
aligned [Neural Amp Modeler (NAM)](https://github.com/sdatkinson/neural-amp-modeler)
reamp recordings through external hardware.

## Overview

Capture Panel plays a NAM training WAV or another source WAV through a selected
ASIO output, records the external return, verifies the route, and removes the
measured round-trip delay from the saved capture.

```text
Source WAV -> ASIO output -> External hardware -> ASIO input -> Aligned WAV
```

The desktop app provides a guided workflow for interactive captures. Its
bundled `capture-panel.exe` worker also provides the same device inspection,
setup verification, and capture operations as a standalone CLI.

Key features:

- Select one full-duplex ASIO driver and independent output/input channels.
- Test input level, latency, clipping, and alignment stability before capture.
- Apply playback and recording trim.
- Save a WAV aligned to the beginning and duration of the source.
- Contain vendor ASIO drivers in a short-lived worker process outside the UI.
- Cancel safely and avoid promoting partial captures to the final output path.

## Installation

Capture Panel requires 64-bit Windows and the audio device vendor's 64-bit ASIO
driver. It is distributed as a portable app, so Capture Panel itself does not
have an installer and does not require administrator access.

1. Download `capture-panel-windows-vX.Y.Z.zip` from
   [GitHub Releases](https://github.com/gmlwhdtjd/Capture-Panel-Windows/releases/latest).
   The similarly named `-source.zip` file contains source code and is not the
   runnable application.
2. Extract the complete ZIP to a writable folder.
3. Install or update the hardware vendor's 64-bit ASIO driver. Its installer
   may require administrator access.
4. Run `CapturePanel.exe`.

Keep the release folder together: the desktop app launches the adjacent
`capture-panel.exe` as its isolated audio worker. Release packages are
self-contained and do not require a separate .NET or Visual C++ runtime
installation.

Current builds are not code-signed, so Microsoft Defender SmartScreen may show
a warning on first launch. Only continue if the ZIP came from this repository's
official GitHub Release and, when needed, verify it against the published
`SHA256SUMS`. In the warning, select **More info**, confirm the file name and
download source, then select **Run anyway**.

## Usage

Connect the selected playback output to the external hardware, then connect the
hardware return to the selected recording input. Start with a conservative
playback level because setup verification produces an audible test signal.

1. Click **Browse...** and choose a source WAV.
2. Select the **Playback** ASIO driver and output channel.
3. Select the **Recording** input channel. The recording driver mirrors the
   playback driver because one ASIO driver owns both sides of the route.
4. Adjust playback or recording trim if needed.
5. Click **Test** and review:
   - **Level** for the recorded signal and clipping.
   - **Latency** for the measured round-trip delay.
   - **Stability** for alignment reliability.
6. When the status changes to **Ready**, click **Capture**, choose the output
   location, and wait for the aligned WAV to be saved.

Changing the source, driver, either channel, or either trim invalidates the
previous result. Run **Test** again before capturing.

## Troubleshooting

- **No ASIO driver appears:** Install the audio device vendor's 64-bit ASIO
  driver, reconnect the device, then use the refresh button. Generic Windows
  audio drivers are not ASIO drivers.
- **The driver is unavailable or busy:** Close DAWs and other applications that
  may be using the ASIO driver, then refresh or restart Capture Panel.
- **Capture is disabled:** Select a valid source and route, run **Test**, and
  wait for **Ready**. Any source, driver, channel, or trim change requires a new
  test.
- **The test reports a missing or low signal:** Check the physical/internal
  route, output channel, and input channel before increasing gain.
- **The sample rate is rejected:** Use a source WAV rate supported by the
  selected ASIO driver or change the device configuration outside Capture
  Panel.

## CLI

Open PowerShell in the extracted release folder. The CLI is the same
`capture-panel.exe` used by the desktop application:

```powershell
.\capture-panel.exe --version
.\capture-panel.exe --help
```

### Inspect drivers and channels

List registered ASIO drivers, then inspect a selected driver ID:

```powershell
.\capture-panel.exe devices
.\capture-panel.exe channels --driver 'asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'
```

Driver IDs are stable registry identities reported by `devices`.

### Test a route

`test` uses a built-in verification signal, so it does not require a source
WAV:

```powershell
.\capture-panel.exe test `
  --driver 'asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' `
  --play-channel 1 `
  --record-channel 1 `
  --verbose
```

### Capture a WAV

```powershell
.\capture-panel.exe run `
  --input source.wav `
  --output captured.wav `
  --driver 'asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' `
  --play-channel 1 `
  --record-channel 1
```

Channel specifications are 1-based and accept individual channels,
comma-separated lists, ranges, or combinations: `1`, `1,2`, `1-4`, or
`1,3-4`. Output trim ranges from -24 through 0 dB, and input trim ranges from
-18 through +12 dB.

By default, the output bit depth matches the source WAV. Use `--bit-depth 16`,
`--bit-depth 24`, or `--bit-depth 32` to override it. Run
`.\capture-panel.exe help <command>` for the complete option list.

## Audio compatibility

- Source WAV: PCM 16/24/32-bit or IEEE float32, including supported
  `WAVE_FORMAT_EXTENSIBLE` files.
- Output WAV: PCM 16/24/32-bit classic RIFF/WAVE.
- Sample rate: the source rate must be supported by the selected ASIO driver.
- Public channel numbers are 1-based; ASIO backend indices are 0-based.

Classic RIFF/WAVE has a 4 GiB container limit. Capture Panel rejects an output
that would exceed the limit before opening the audio driver. RF64 and BW64 are
not currently supported.

## Development

Requirements are Visual Studio 2026 with the **Desktop development with C++**
and **.NET desktop development** workloads, CMake 4.2 or newer, a Windows SDK,
and the .NET 10 SDK selected by `global.json`.

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
```

Main outputs:

- Desktop app: `out\dotnet\CapturePanel.App\<Configuration>\net10.0-windows\win-x64\CapturePanel.exe`
- Bundled worker: the adjacent `capture-panel.exe`
- Standalone CLI: `out\build\windows-x64\bin\<Configuration>\capture-panel.exe`

For development without hardware, set `CAPTURE_PANEL_SHOW_FAKE=1` before
launching to expose the deterministic Fake device in the desktop app.

```powershell
$env:CAPTURE_PANEL_SHOW_FAKE = '1'
& '.\out\dotnet\CapturePanel.App\Debug\net10.0-windows\win-x64\CapturePanel.exe'
```

## Release automation

A pushed `vX.Y.Z` tag matching both application versions triggers the release
workflow. It tests native and managed code, publishes a self-contained
`win-x64` folder, places the worker beside the desktop app, and creates a ZIP
with licenses, third-party notices, source archive, and SHA-256 checksums.
Hardware vendor drivers are never redistributed.

## License

Copyright (C) 2026 Lee Hui-Jong.

Capture Panel for Windows is free software licensed under
[GNU GPL v3.0 only](LICENSE). The Steinberg ASIO SDK interface files use their
GPLv3 alternative; preserved notices and details are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Hardware vendor drivers are
not distributed by this project.
