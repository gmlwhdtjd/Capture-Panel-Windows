# Development

## Prerequisites

- Visual Studio 2026 Community or Build Tools
- Desktop development with C++ workload
- CMake 4.2 or newer
- Windows SDK
- PowerShell 7 or Windows PowerShell 5.1

.NET 10 and the .NET desktop workload are needed only when the WPF milestone begins.

## Configure, build, and test

```powershell
.\build.ps1 -Configuration Debug -Test
```

`build.ps1` uses CMake from `PATH` when available and otherwise locates the
Visual Studio bundled CMake through `vswhere`.

Release validation:

```powershell
.\build.ps1 -Configuration Release -Test
```

Clean rebuild:

```powershell
.\build.ps1 -Configuration Debug -Test -Clean
```

## Development rules

- Put platform-independent behavior in `capture_panel_core`.
- Add Windows/ASIO code only under `src/backends/asio`.
- Keep the Fake backend behavior deterministic.
- Do not allocate, log, perform file I/O, or call managed code from a future ASIO callback.
- Add or port a test before changing alignment and verification constants.
- Public channel numbers are one-based; backend indices are zero-based.
- Core audio buffers are normalized interleaved float32.
- Do not commit `out/`, generated Visual Studio files, or `CMakeUserPresets.json`.
