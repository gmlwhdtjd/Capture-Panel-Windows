# ASIO Hardware Testing

Physical tests are separate from hosted CI. They play a real electrical signal,
so confirm the route and start at a low level.

## Darkglass Anagram target

Tested on 2026-07-10:

- official Darkglass USB Audio driver 5.72.0 (2025-04-14);
- x64 ASIO DLL 5.72.0.0 with a valid Thesycon code signature;
- driver ID `asio:{60FEF341-94DE-4E97-A4A2-1C355DC8AD61}`;
- 3 inputs, 9 outputs, 48 kHz;
- output 9 `Virtual Input` -> input 1 `Processed L`;
- internal ASIO indices: output `channelNum = 8`, input `channelNum = 0`.

The vendor driver is not included in this repository. Install the current
official 64-bit driver from the
[Darkglass Suite page](https://www.darkglass.com/products/darkglass-suite) as
an administrator.

## Safety checklist

1. Close DAWs and any other application that may hold the ASIO driver.
2. Mute or disconnect monitors, headphones, amplifiers, and unrelated outputs.
3. Confirm the intended Anagram internal/physical return path.
4. Set hardware input and output gains conservatively.
5. List channels instead of guessing their numbers.
6. Start with `--output-trim -24`. The verification signal is nominally
   -12 dBFS, so the expected maximum output is about -36 dBFS.

## Discover the installed driver

```powershell
$cli = '.\out\build\windows-x64\bin\Debug\capture-panel.exe'

& $cli devices
$driver = 'asio:{COPY-THE-DARKGLASS-ID-FROM-DEVICES}'
& $cli channels --driver $driver
```

Do not continue unless output 9 and input 1 exist with the expected names.

## Low-level verification

```powershell
& $cli test `
  --driver $driver `
  --play-channel 9 `
  --record-channel 1 `
  --output-trim -24 `
  --verbose
```

Stop immediately if an unintended output is active. If the return is missing,
check the Anagram routing, cable/mixer, and channel assignment first. Raise the
level only in small steps such as 6 dB.

A good pass reports five markers, no clipping or missing-signal failure, a
reliable sweep, finite latency, small timing error, and no warnings.

## Recorded result

Three Debug passes and four Release passes on the tested setup succeeded:

| Pass | Build | Output peak | Input peak | Marker latency | Timing error | Warnings/failures |
|---|---|---:|---:|---:|---:|---:|
| 1 | Debug | -36.00 dBFS | -35.99 dBFS | 344 frames / 7.17 ms | 0 frames | 0 / 0 |
| 2 | Debug | -36.00 dBFS | -35.99 dBFS | 338 frames / 7.04 ms | 0 frames | 0 / 0 |
| 3 | Debug | -36.00 dBFS | -35.99 dBFS | 312 frames / 6.50 ms | 0 frames | 0 / 0 |
| 4 | Release | -36.00 dBFS | -35.99 dBFS | 348 frames / 7.25 ms | 0 frames | 0 / 0 |
| 5 | Release | -36.00 dBFS | -35.99 dBFS | 330 frames / 6.88 ms | 0 frames | 0 / 0 |
| 6 | Release | -36.00 dBFS | -35.99 dBFS | 340 frames / 7.08 ms | 0 frames | 0 / 0 |
| 7 | Release (/MT) | -36.00 dBFS | -35.99 dBFS | 338 frames / 7.04 ms | 0 frames | 0 / 0 |

All seven detected all five markers with direct correlation 1.0000.

## Capture a WAV

Only after the setup test passes:

```powershell
& $cli run `
  --input .\source.wav `
  --output .\recorded.wav `
  --driver $driver `
  --play-channel 9 `
  --record-channel 1 `
  --output-trim -24 `
  --verbose
```

Inspect the written WAV before increasing to the final approved level.

## Troubleshooting and test records

- `unavailable`: close other ASIO hosts and verify/reinstall the official driver.
- reset/resync/overload: inspect USB stability and driver buffer settings, then
  start a new command.
- unexpected channel names/counts: stop; do not infer a replacement number.
- missing signal: inspect routing before raising trim.
- clipping: lower output/hardware gain and repeat.

Record the application commit, driver/firmware version, driver ID, channel
names, sample rate, buffer settings, trim, peaks, latency, warnings, and result.

## CI boundary

Hosted tests compile/link the ASIO backend and validate every converter format,
device-ID parsing, backend routing, core algorithms, CLI behavior, and the full
Fake capture flow. They do not load a vendor driver or validate USB scheduling,
physical routing, levels, reset/disconnect behavior, or long-duration stability.
Those remain a manual or protected self-hosted hardware gate.
