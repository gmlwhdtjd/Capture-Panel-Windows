# Streaming Audio Architecture

## Scope

The live capture path keeps memory use bounded as the source duration grows.
Source decoding and recording-file writes run on worker threads, while the ASIO
callback exchanges only preallocated interleaved Float32 blocks through bounded
single-producer/single-consumer (SPSC) rings.

Setup verification intentionally uses a short in-memory signal. Arbitrary WAV
captures use the streaming path described here.

## End-to-end flow

```text
source WAV
  -> metadata-only RIFF validation
  -> chunked peak scan
  -> CaptureAudioSource / CapturePassPlaybackPlan

playback worker
  -> marker and silence preamble
  -> chunked WAV decoder
  -> playback gain
  -> playback SPSC ring
  -> ASIO output callback

ASIO input callback
  -> selected-channel conversion into preallocated scratch
  -> recording SPSC ring
  -> recording writer worker
  -> temporary interleaved Float32 file

temporary Float32 asset
  -> bounded marker-window alignment
  -> AlignedCapturePayload (start frame, frame count, gain)
  -> chunked PCM WAV encoder
  -> sibling temporary WAV
  -> atomic destination promotion
```

## Source and WAV validation

`read_wav_format` parses the RIFF container and returns metadata without
materializing the audio payload. `wav_peak_level` then decodes bounded chunks,
tracks only the running peak, observes cancellation, and verifies that the
declared payload can be read completely.

A WAV-backed source retains a file-size and last-write-time snapshot. Reader
creation, end-of-source, and capture phase boundaries validate that snapshot so
an accidental source replacement or edit fails closed. The WPF boundary adds a
stronger SHA-256 and Windows file-identity check before and after each native
worker operation.

Output is classic RIFF/WAVE, not RF64 or BW64. Before an audio device is opened,
the core checks sample rate, block alignment, byte rate, encoded data size,
padding, and the 32-bit RIFF chunk-size limit. The streaming writer repeats the
same validation before touching the destination. Existing output remains intact
unless a complete temporary WAV is successfully closed and promoted.

## ASIO streaming

The ASIO backend owns two preallocated SPSC rings. Each ring can hold at least
eight driver blocks or approximately half a second of audio, whichever is
larger. The playback worker fills a startup prebuffer before the driver starts.

The playback worker emits alignment markers and silence, reads the source in
8,192-frame chunks, applies playback gain, and feeds the playback ring. The
recording writer drains the second ring into a create-new `.f32` scratch file
and calculates the raw input peak while writing.

The final input callback writes its exact logical frame block before publishing
producer completion. The writer observes that publication, rechecks and drains
the ring, validates the exact expected frame count, flushes and closes the file,
and only then publishes completion. A successful result therefore cannot omit
the last callback block or hide a late disk-close failure.

Playback underflow, recording overflow, source-read failure, recording-write
failure, cancellation, timeout, and fatal driver notifications are explicit
terminal outcomes. Missing playback frames and discarded recording frames are
never reported as success.

## Real-time callback contract

All vectors, conversion views, scratch buffers, and rings used by the callback
are allocated before `IASIO::start`. The callback is limited to:

- clearing and copying preallocated memory;
- bounded sample and channel conversion loops;
- exact SPSC reads and writes;
- lock-free atomic state and progress publication; and
- `outputReady` when the driver supports it.

It does not perform file I/O, allocate or resize containers, acquire a lock,
sleep, log, invoke application progress handlers, or throw.

The control thread pumps the hidden ASIO host window, reports progress, observes
cancellation and timeouts, and owns driver teardown. Cleanup order is:

```text
stop driver
  -> close and drain callback lifetime gate
  -> stop/join stream workers
  -> dispose ASIO buffers
  -> release driver/session resources
```

On a completed capture the recording writer is allowed to drain and close before
its join. On every abnormal path both workers are explicitly woken before join,
so a producer waiting on a full ring or a writer waiting on an empty ring cannot
deadlock teardown.

## Alignment and output

Alignment does not load the raw recording in full. It reads only the bounded
marker search window needed by the impulse detector. A successful alignment
returns a descriptor over the raw asset: start frame, requested frame count,
and recording gain.

The aligned reader seeks to that start, applies gain per chunk, and zero-pads a
short captured tail. The WAV encoder consumes exactly the declared frame count
in bounded chunks and observes cancellation between reads and writes.

Normal success, failure, and cancellation release both the raw recording and
any incomplete output sibling through RAII. In the WPF application the native
worker writes to a short, hidden staging filename; managed cleanup also removes
its matching raw sibling after a forced worker termination.

Long captures temporarily need enough disk space for both the interleaved
Float32 scratch recording and the encoded WAV. Free space can change after the
RIFF size preflight, so create, write, flush, close, and promotion failures all
remain explicit capture errors.
