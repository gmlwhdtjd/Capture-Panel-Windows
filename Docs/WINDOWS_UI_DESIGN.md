# Windows UI Design

## Status and source of truth

This document defines the first Windows desktop UI milestone. It is based on
the macOS `CapturePanelView`, `CapturePanelViewModel`, verification
presentation, settings window, and their application tests. Where Windows and
macOS audio routing differ, this document takes precedence.

The implementation target is C# WPF on .NET 10, x64. The native C++ core, CLI,
Fake backend, and ASIO backend remain the audio source of truth.

## Product goals

- Preserve the macOS workflow and information hierarchy.
- Require a successful setup test before enabling a real capture.
- Keep ASIO and all vendor driver code outside the WPF process.
- Make device loss, worker failure, cancellation, and retry visible and safe.
- Persist the last source, driver, channels, and level trims locally.
- Include GPLv3 and third-party license information in About.

The first UI selects one playback channel and one recording channel. The core
and worker protocol retain list-based channel fields so multichannel UI can be
added later without changing the protocol shape.

## Windows audio constraint

An ASIO driver exposes its playback and recording channels as one synchronous
device. Capture Panel for Windows does not combine unrelated devices.

The two device panels therefore behave as follows:

- The Playback device combo box is the only editable device selector.
- Selecting Playback also selects the same driver for Recording.
- The Recording device combo box is always disabled and displays the selected
  Playback driver.
- Playback and Recording channel selectors remain independently editable.
- Aggregate-device, drift compensation, and clock-source controls are omitted.
- Changing the driver, either channel, either trim, or the source invalidates a
  previous setup test.

The disabled Recording selector must expose the accessible help text:
`ASIO uses one driver for playback and recording.`

## Process architecture

```text
CapturePanel.exe (WPF)
  |-- MainViewModel and SettingsWindow
  |-- local JSON settings
  +-- WorkerClient
        +-- one capture-panel.exe process per operation
              |-- versioned JSON Lines protocol
              |-- CaptureService
              |-- BackendRouter
              +-- ASIO or Fake backend
```

The WPF application does not load `IASIO` or a native capture DLL. ASIO drivers
are in-process COM servers; keeping them in a short-lived worker process
contains vendor access violations and lets the UI terminate a hung operation.
Audio samples never cross the process boundary. Only device metadata, progress,
diagnostics, and final results are serialized.

The public CLI text format remains human-readable. UI integrations use
`--json`, whose protocol identifier is `capture-panel/1`. Each JSON object is a
single UTF-8 line, and stdout contains no non-protocol text in this mode.
The Test result's `inputPeakDbfs` is measured after input trim. Raw or
post-trim clipping is carried independently as a `digital_clipping` diagnostic,
so the UI does not infer clipping from the adjusted number.

Cancellation terminates the isolated worker process, which also contains a
hung or faulting vendor driver. A real capture is written to a temporary file
in the selected destination directory; the UI moves it to the final filename
only after a successful worker result and a final cancellation check.

## Main window

The macOS application defines the workflow and section order, not the Windows
visual language. The Windows application uses the .NET 10 WPF Fluent theme in
system light/dark mode and keeps the native Button, ComboBox, Slider,
ProgressBar, ScrollBar, focus, hover, disabled, and keyboard behavior.

The main window is fixed at 600 x 740 device-independent pixels. It can be
minimized or closed but cannot be resized or maximized. All functional content
remains vertically scrollable for text scaling and accessibility. Cards are
used only for top-level sections; controls inside a section use spacing and
dividers instead of nested cards.
The compact density uses a 16 DIP page inset, 8 DIP between cards, 16 DIP card
padding, 12 DIP status padding, and 32 DIP input controls. Repeated rows inside
Playback and Recording use 10-12 DIP rather than dashboard-scale whitespace.
Segoe Fluent Icons, with Segoe MDL2 Assets as the fallback, are limited to the
Test and Capture/Cancel action buttons. Capture uses a red record or cancel
glyph. Gear and Refresh keep their existing Windows glyphs. Source/Browse,
verification metrics, section titles, ASIO fields, channel fields, sliders,
meters, and status text remain text-only.

The page keeps the macOS order and information set: operation status, source
commands, setup verification, Playback, and Recording. It does not add a page
header, helper paragraphs, section subtitles, or visible platform-explanation
badges that are absent from the original workflow.

### 1. Operation status

A Windows information panel contains:

- a neutral dot for normal configuration steps such as selecting a source or
  running the first Test;
- a constant neutral card background, with only the dot and border changing to
  blue while active, green when ready, amber for warnings, and red for a real
  failure or unavailable device;
- title: `Select Device`, `Choose Source`, `Run Test`, `Testing`, `Ready`,
  `Capturing`, or `Cancelling`;
- one-line guidance or progress text;
- the original settings affordance, which opens About and licenses;
- a semantic ProgressBar rendered as a subtle full-card background fill during
  Test and Capture, matching the macOS behavior without changing card height.

### 2. Source and primary actions

- a non-interactive display field shows the selected WAV filename without
  text-entry focus, selection, or copy behavior;
- a separate `Browse...` button opens the file picker;
- Test: enabled when source, available driver, and both channels are selected.
- Capture: enabled only after the current setup has passed Test.
- Enabled Capture uses a solid critical-red fill with white label and glyph;
  disabled Capture returns to the neutral disabled-button appearance.
- Capture changes to Cancel while a capture worker is active.
- Route and source controls are locked while Test or Capture is active.

The default output name is `<source-base-name>-captured.wav`.

### 3. Setup verification

Three equally sized metrics remain in one flat row separated by Windows divider
lines. They preserve the macOS values without reproducing its nested result
pills:

| Pill | Values |
|---|---|
| Level | `-`, `Low`, `Normal`, `Hot`, or `Clipping` |
| Latency | `-` or marker latency in milliseconds |
| Stability | `-`, `Excellent`, `Good`, `Caution`, `Unstable`, or `Failed` |

Stability is derived from absolute verification timing error:

| Timing error | Level |
|---|---|
| <= 0.5 ms | Excellent |
| <= 2 ms | Good |
| <= 5 ms | Caution |
| > 5 ms | Unstable |

Clipping or a missing verification signal is `Failed`. Equipment decay, low
marker evidence, high alignment-fit error, or an ambiguous verification match
raises the result to at least `Caution`. A timing mismatch raises it to at least
`Unstable`.

Input Level uses the same thresholds as macOS: below -24 dBFS is `Low`, from
-24 dBFS through values below -6 dBFS is `Normal`, and -6 dBFS or above is
`Hot`. `digital_clipping` takes precedence over those ranges. Low and Hot are
amber, Normal is accent blue, and Clipping is critical red. Low and Hot alone
do not fail Test or disable Capture.

### 4. Playback

- editable ASIO driver combo box;
- independently editable output-channel combo box;
- output trim slider from -24 dB to 0 dB in 1 dB steps;
- measured output peak meter and dBFS text.

### 5. Recording

- non-interactive driver display synchronized with Playback, without a dropdown
  arrow or selection affordance;
- independently editable input-channel combo box;
- input trim slider from -18 dB to +12 dB in 1 dB steps;
- post-input-trim peak meter and dBFS text. Clipping still checks both the raw
  recording and the adjusted signal.

Meters cover -60 dBFS to 0 dBFS. Values above -6 dBFS are orange and values
above -1 dBFS are red.

## State and operation rules

```text
No device/source
  -> configuration selected
  -> Test running
  -> Test passed (Capture enabled)
  -> Capture running
  -> Saved or Cancelled

Any source/driver/channel/trim change
  -> Test required again

Device loss or worker failure
  -> Capture disabled, error shown, Refresh/Retry available
```

Only one Test or Capture operation may run at a time. Device discovery and
channel refresh do not run concurrently with an audio operation.

Device and channel discovery use 20-second watchdogs. Setup Test uses a
30-second watchdog. Closing the window cancels discovery, channel lookup, Test,
and Capture workers so no child process keeps an ASIO driver open.

The UI treats a failed Test as a valid diagnostic result when the worker emits
one. A worker crash, protocol mismatch, timeout, or unstructured exit is an
operation error and never enables Capture.

## Persistence

Settings are stored as JSON under the current user's local application-data
directory. Persisted values are:

- source path;
- ASIO driver ID and display name;
- playback and recording channel indices;
- output and input trim values.

If the saved source is missing, it is cleared. If the saved driver is installed
but unavailable, it remains visible with its diagnostic and blocks Test. Device
IDs, not display names, are the stable selection key.

## About and licensing

The About window contains:

- application version;
- native worker path and availability;
- `GNU GPL version 3 only` application license notice;
- Steinberg ASIO SDK third-party notice;
- buttons to open `LICENSE` and `THIRD_PARTY_NOTICES.md`.

Windows does not install a separate global CLI symlink, so the macOS Command
Line Tool installer section is replaced by bundled-worker status. License
actions use full-width native Windows command rows instead of a wrapping button
strip.

## Accessibility

- Every combo box, slider, primary button, result pill, and meter has an
  automation name.
- Disabled state is never communicated by color alone.
- Status and result text remains available to screen readers.
- Visual progress is throttled to 10 Hz and status live-region announcements
  are coalesced to at most once per second.
- Keyboard tab order follows visual order.
- The Recording device explanation is exposed as help text.
- Layout remains usable at 200 percent Windows scaling.

## Verification plan

Automated tests use a fake managed worker client and verify:

- Playback selection always synchronizes the disabled Recording device;
- source, route, and trim changes invalidate Test;
- Test pass, clipping, timing mismatch, and missing-signal presentation;
- progress, cancellation, worker error, and retry state transitions;
- persisted settings restoration and missing saved files;
- JSON protocol parsing, Unicode names/paths, and error objects.

Native tests cover JSON output for `devices`, `channels`, `test`, and `run` with
the Fake backend. CI builds both Debug and Release WPF applications without
opening a vendor ASIO driver.

Manual UI validation covers:

- Darkglass Anagram output 9 to input 1;
- Focusrite output 1 to input 1;
- USB disconnect during Test and Capture;
- reconnect, Refresh, and immediate retry;
- full T3K capture and cancellation without a partial final output file.

## Build and distribution

`build.ps1` builds the native C++ worker before the WPF project so the managed
project can copy `capture-panel.exe` beside `CapturePanel.exe`. With `-Test`, it
also runs both CTest and the dependency-free managed UI test executable.

Debug application output:

```text
out/dotnet/CapturePanel.App/Debug/net10.0-windows/win-x64/
|-- CapturePanel.exe
|-- capture-panel.exe
|-- LICENSE
|-- THIRD_PARTY_NOTICES.md
+-- licenses/Steinberg-ASIO-SDK-LICENSE.txt
```

Tagged releases publish the WPF application as an untrimmed, self-contained
`win-x64` folder and copy the Release CLI into the same directory as the GUI.
The combined binary ZIP therefore supports both double-click desktop use and
standalone CLI use without a preinstalled .NET or Visual C++ runtime. The exact
tagged source ZIP and SHA-256 manifest are published beside it. Vendor ASIO
drivers are not redistributed.
