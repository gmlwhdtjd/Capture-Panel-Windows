[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$SourcePath,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$DestinationPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = [IO.Path]::GetFullPath($SourcePath)
$destination = [IO.Path]::GetFullPath($DestinationPath)
if ($source.Equals($destination, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The bundled README destination must differ from the repository README.'
}

$gplSourceLink = '[GNU GPL v3.0 only](LICENSE)'
$gplBundledLink = '[GNU GPL v3.0 only](../licenses/GPL-3.0.txt)'
$noticesSourceLink = '[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)'
$noticesBundledLink = '[THIRD_PARTY_NOTICES.md](../licenses/THIRD_PARTY_NOTICES.md)'

$text = [IO.File]::ReadAllText($source)
if (-not $text.Contains($gplSourceLink) -or
    -not $text.Contains($noticesSourceLink)) {
    throw 'The repository README legal links changed; update the bundled README transformation.'
}

$text = $text.Replace($gplSourceLink, $gplBundledLink)
$text = $text.Replace($noticesSourceLink, $noticesBundledLink)

$directory = [IO.Path]::GetDirectoryName($destination)
if ([string]::IsNullOrWhiteSpace($directory)) {
    throw "The bundled README destination has no parent directory: $destination"
}

[IO.Directory]::CreateDirectory($directory) | Out-Null
[IO.File]::WriteAllText($destination, $text, [Text.UTF8Encoding]::new($false))
