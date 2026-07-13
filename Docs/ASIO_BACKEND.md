# Native ASIO Backend

## Identity and discovery

The backend enumerates the 64-bit `HKLM\SOFTWARE\ASIO` registry view. A valid
registration becomes a stable device ID:

```text
asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
```

Unavailable registrations remain visible with their initialization diagnostic.
This distinguishes "installed but disconnected or busy" from "not installed."
Capture Panel is x64-only and does not enumerate 32-bit-only COM servers.

## Driver session

Every operation creates the driver on its calling control thread in an STA COM
apartment and supplies a hidden top-level `HWND` to `IASIO::init`. Capture keeps
that thread alive and pumps Windows messages until streaming finishes. The same
thread stops, disposes, and releases the driver.

All public operations that touch a driver share a process-wide session lease.
This covers probing and sample-rate changes as well as streaming; same-thread
re-entry fails immediately instead of deadlocking.

The implementation calls the `IASIO` COM vtable directly and does not use the
SDK's legacy global host helpers.

## Channel mapping

CLI/core channel numbers are one-based. ASIO channel indices are zero-based.
For the tested Anagram route:

```text
physical output 9 -> ASIO output channelNum 8  (Virtual Input)
physical input 1  -> ASIO input  channelNum 0  (Processed L)
```

Playback source channels map by route order. Selected input channels become the
recorded interleaved channel order. Duplicate physical channels are rejected.

## PCM formats

The converter supports:

- signed Int16, Int24, and Int32, little- and big-endian;
- Float32 and Float64, little- and big-endian;
- right-aligned Int32 containers with 16, 18, 20, or 24 valid bits.

Input/output floats are normalized to `[-1, 1]`. Integer output is clamped and
quantized; NaN becomes silence. DSD and unknown sample types fail before the
stream starts.

## Buffer timeline and real-time path

Before `createBuffers`, a playback worker fills a bounded SPSC ring with at
least four driver blocks or approximately a quarter second of audio. After
`createBuffers`, both output halves are cleared. Output half B (index 1) is then
filled with the first logical block before `start`, as required by the ASIO
double-buffer timeline. The first input A callback is invalid and is skipped
without advancing the recorded cursor. Subsequent callbacks write the next
output block and read the current input block. Once playback ends, output
buffers remain zero while input drains. The backend stops only after the
requested recorded frame count is complete.

Before `start`, Capture Panel allocates both SPSC rings, callback scratch blocks,
descriptors, and conversion views. The callback only:

1. reads one exact playback block from the playback ring and writes a selected
   output half from preallocated memory;
2. converts a selected input half into preallocated scratch and writes one exact
   block to the recording ring;
3. publishes progress and driver events through lock-free atomics; and
4. calls `outputReady` when the driver reports support.

The callback never allocates, locks, logs, performs file I/O, invokes a progress
handler, or throws. A separate writer drains the recording ring into a
create-new temporary Float32 file and computes the raw peak while writing. See
[Streaming Audio Architecture](STREAMING_AUDIO_ARCHITECTURE.md) for the full
data flow and ownership rules.

## Control path and failures

The control loop pumps the host window and polls callback state. Timeout is
`max(10 seconds, requested duration + 5 seconds)`. Cancellation uses the core
cancellation token.

A playback underflow, recording overflow, source-read failure, temporary-file
write/close failure, reset request, resync request, sample-rate change, overload,
invalid buffer index, reentrant callback, or conversion failure stops and
disposes the stream before returning a `CaptureError`. A latency-change notice
is refetched on the control thread. The driver's preferred buffer size is used.

Because ASIO callbacks have no host context pointer, their target is published
through a lock-free reader gate. Every cleanup path stops the stream, closes the
gate and waits for readers, wakes and joins both stream workers, then disposes
buffers. A successful path lets the writer drain the final published callback
block and close the scratch file before joining. Cleanup is armed before
`createBuffers` and `start`, so a driver that partially succeeds before
returning an error cannot leave host buffers active.

The source sample rate is applied before capture and verified by reading it
back. Capture orchestration restores the previous rate on a best-effort basis.
Marker-based physical-loop measurement, rather than the driver's reported
latency alone, determines final alignment.

## Tested device

On 2026-07-10, Darkglass USB Audio 5.72.0 exposed 3 inputs, 9 outputs, and
48 kHz. Three Debug and four Release low-level output 9 -> input 1 verification
passes completed with no warnings or failures. Measured marker latency ranged
from 312 to 348 frames (6.50 to 7.25 ms).

This record predates the bounded-ring streaming refactor. Deterministic tests
cover its data flow and teardown; a new physical-device pass remains a release
gate.

## SDK and licensing

The repository vendors the interface headers needed to compile the host from
Steinberg ASIO SDK 2.3.4. The official archive URL, size, and SHA-256 are
recorded in `third_party/asio/README.md`. Capture Panel uses the SDK under its
GPLv3 alternative. Header notices and the SDK license are preserved.

No hardware vendor driver is included or redistributed.

## Known isolation limit

ASIO drivers are in-process COM servers. `devices` initializes each registered
64-bit driver to report live channels and availability. A defective third-party
driver that hangs or crashes during initialization/callback cannot be fully
isolated by C++ exception handling and may affect a direct CLI process. The WPF
application contains this risk by starting a short-lived `capture-panel.exe`
worker for each operation and terminating an unresponsive worker. The tested
Darkglass, Realtek, and Focusrite registrations completed enumeration on this
machine.
