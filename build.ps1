[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$preset = if ($Configuration -eq 'Release') { 'windows-release' } else { 'windows-debug' }
$buildDirectory = Join-Path $PSScriptRoot 'out\build\windows-x64'

# Some application hosts can inject both `Path` and `PATH`. MSBuild treats the
# process environment case-insensitively and fails before invoking cl.exe when
# both spellings exist. Keep the canonical Windows spelling in this process.
$environment = [System.Environment]::GetEnvironmentVariables()
$hasCanonicalPath = @($environment.Keys | Where-Object { [string]$_ -ceq 'Path' }).Count -gt 0
$hasUpperPath = @($environment.Keys | Where-Object { [string]$_ -ceq 'PATH' }).Count -gt 0
if ($hasCanonicalPath -and $hasUpperPath) {
    [System.Environment]::SetEnvironmentVariable(
        'PATH',
        $null,
        [System.EnvironmentVariableTarget]::Process
    )
}

function Resolve-CMakeTool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.CMake.Project `
            -property installationPath
        if ($installation) {
            $candidate = Join-Path $installation `
                "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$Name.exe"
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    throw "$Name was not found. Install Visual Studio 2026 Desktop development with C++."
}

$cmake = Resolve-CMakeTool 'cmake'
$ctest = Resolve-CMakeTool 'ctest'

if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) {
    $repositoryRoot = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $resolvedBuildDirectory = [System.IO.Path]::GetFullPath($buildDirectory)
    if (-not $resolvedBuildDirectory.StartsWith(
            "$repositoryRoot\",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $resolvedBuildDirectory"
    }
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
}

& $cmake --preset windows-x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & $ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
