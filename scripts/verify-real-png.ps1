[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PngPath,

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',

    [string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot 'build'
}

$smokeExecutable = Join-Path $BuildDirectory "$Configuration\ZTFrameUploadSmoke.exe"
if (-not (Test-Path -LiteralPath $smokeExecutable -PathType Leaf)) {
    throw "Frame upload smoke executable was not found: $smokeExecutable"
}

$resolvedPngPath = (Resolve-Path -LiteralPath $PngPath).ProviderPath
if (-not [System.IO.Path]::GetExtension($resolvedPngPath).Equals('.png', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Validation input must be a PNG file: $resolvedPngPath"
}

function Invoke-FrameUploadSmoke {
    param([uint32]$Percent)

    $output = @(& $smokeExecutable $resolvedPngPath $Percent 2>&1)
    $exitCode = $LASTEXITCODE
    foreach ($line in $output) {
        Write-Host $line
    }
    if ($exitCode -ne 0) {
        throw "Frame upload smoke failed at $Percent percent with exit code $exitCode"
    }

    $text = $output -join [Environment]::NewLine
    $match = [regex]::Match($text, 'frame=(\d+)x(\d+)\s+stride=(\d+)\s+bytes=(\d+)')
    if (-not $match.Success) {
        throw "Frame upload smoke output could not be parsed at $Percent percent"
    }

    $width = [uint32]$match.Groups[1].Value
    $height = [uint32]$match.Groups[2].Value
    $stride = [uint64]$match.Groups[3].Value
    $bytes = [uint64]$match.Groups[4].Value
    if ($stride -ne ([uint64]$width * 4) -or $bytes -ne ($stride * $height)) {
        throw "Invalid BGRA layout reported at $Percent percent"
    }

    return [pscustomobject]@{
        Percent = $Percent
        Width = $width
        Height = $height
    }
}

$fullSize = Invoke-FrameUploadSmoke -Percent 100
foreach ($percent in @(25, 50, 75)) {
    $result = Invoke-FrameUploadSmoke -Percent $percent
    $expectedWidth = [uint32][Math]::Max(1, [Math]::Floor((([double]$fullSize.Width * $percent) + 50.0) / 100.0))
    $expectedHeight = [uint32][Math]::Max(1, [Math]::Floor((([double]$fullSize.Height * $percent) + 50.0) / 100.0))
    if ($result.Width -ne $expectedWidth -or $result.Height -ne $expectedHeight) {
        throw "Unexpected dimensions at $percent percent: $($result.Width)x$($result.Height), expected ${expectedWidth}x${expectedHeight}"
    }
}

Write-Host "Real PNG validation passed for 25, 50, 75, and 100 percent: $resolvedPngPath"
