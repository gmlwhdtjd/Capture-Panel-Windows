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
$managedOutputDirectory = Join-Path $PSScriptRoot 'out\dotnet'
$managedIntermediateDirectory = Join-Path $PSScriptRoot 'out\dotnet-obj'
$managedPublishDirectory = Join-Path $PSScriptRoot 'out\publish\windows-x64'
$uiProject = Join-Path $PSScriptRoot 'src\ui\CapturePanel.App\CapturePanel.App.csproj'
$uiTestsProject = Join-Path $PSScriptRoot 'tests\ui\CapturePanel.App.Tests.csproj'

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

function Resolve-DotnetTool {
    $command = Get-Command 'dotnet' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    throw 'dotnet was not found. Install the .NET 10 SDK and .NET desktop development workload.'
}

function Remove-RepositoryDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }

    $repositoryRoot = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $resolvedPath.StartsWith(
            "$repositoryRoot\",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a directory outside the repository: $resolvedPath"
    }
    Remove-Item -LiteralPath $resolvedPath -Recurse -Force
}

$cmake = Resolve-CMakeTool 'cmake'
$ctest = Resolve-CMakeTool 'ctest'
$dotnet = Resolve-DotnetTool

if (-not (Test-Path -LiteralPath $uiProject) -or
    -not (Test-Path -LiteralPath $uiTestsProject)) {
    throw 'The WPF application or managed test project is missing.'
}

if ($Clean) {
    foreach ($directory in @(
            $buildDirectory,
            $managedOutputDirectory,
            $managedIntermediateDirectory,
            $managedPublishDirectory)) {
        Remove-RepositoryDirectory $directory
    }
}

& $cmake --preset windows-x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & $ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$dotnetVersion = & $dotnet --version
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Using .NET SDK $dotnetVersion"

$nativeCli = Join-Path $buildDirectory "bin\$Configuration\capture-panel.exe"
if (-not (Test-Path -LiteralPath $nativeCli)) {
    throw "Native worker not found after build: $nativeCli"
}
$nativeVersion = ((& $nativeCli --version) -replace '^capture-panel\s+', '').Trim()
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$managedVersion = (& $dotnet msbuild $uiProject -nologo -getProperty:Version).Trim()
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($nativeVersion -ne $managedVersion) {
    throw "Native/WPF version mismatch: native '$nativeVersion', WPF '$managedVersion'."
}

& $dotnet restore $uiProject
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $dotnet build $uiProject --configuration $Configuration --no-restore
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & $dotnet restore $uiTestsProject
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $dotnet build $uiTestsProject --configuration $Configuration --no-restore
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $dotnet run --project $uiTestsProject --configuration $Configuration `
        --no-build --no-restore
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
