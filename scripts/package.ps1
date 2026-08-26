[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',

    [string]$RealPngPath = $env:ZT_SEQUENCE_TEST_PNG,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$buildScript = Join-Path $PSScriptRoot 'build.ps1'

if (-not $SkipBuild) {
    $buildParameters = @{
        Configuration = $Configuration
        Clean = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($RealPngPath)) {
        $buildParameters.RealPngPath = $RealPngPath
    }
    & $buildScript @buildParameters
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'Visual Studio 2022 Build Tools with the MSVC x64 toolchain was not found.'
}

$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw "CMake was not found: $cmake"
}

$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$releaseWord = [string]::Concat([char]0x53D1, [char]0x5E03)
$toolName = Split-Path -Leaf $projectRoot
$outputParent = Split-Path -Parent $projectRoot
$outputDirectory = Join-Path $outputParent ("{0}_{1}_{2}" -f $toolName, $releaseWord, $timestamp)
if (Test-Path -LiteralPath $outputDirectory) {
    throw "Release output already exists: $outputDirectory"
}

& $cmake --install $buildDirectory --config $Configuration --prefix $outputDirectory
if ($LASTEXITCODE -ne 0) {
    throw "CMake install failed with exit code $LASTEXITCODE"
}

$executable = Join-Path $outputDirectory 'FramePlayer.exe'
$notices = Join-Path $outputDirectory 'THIRD_PARTY_NOTICES.md'
$license = Join-Path $outputDirectory 'DEAR_IMGUI_LICENSE.txt'
$readme = Join-Path $outputDirectory 'README.md'
$packageFiles = @($executable, $readme, $notices, $license)
foreach ($file in $packageFiles) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required release file was not installed: $file"
    }
}

$versionFile = Join-Path $projectRoot 'version.txt'
$version = [System.IO.File]::ReadAllText($versionFile).Trim()
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "Invalid version.txt: $version"
}
$zipName = "FramePlayer_${version}_Win64.zip"
$zipPath = Join-Path $outputDirectory $zipName
Compress-Archive -LiteralPath $packageFiles -DestinationPath $zipPath -CompressionLevel Optimal

$hashManifest = Join-Path $outputDirectory 'SHA256SUMS.txt'
$hashTargets = @($executable, $readme, $notices, $license, $zipPath)
$hashLines = foreach ($file in $hashTargets) {
    $hash = Get-FileHash -LiteralPath $file -Algorithm SHA256
    "{0} *{1}" -f $hash.Hash, (Split-Path -Leaf $file)
}
Set-Content -LiteralPath $hashManifest -Value $hashLines -Encoding ASCII

Write-Host "Release package completed: $outputDirectory"
Write-Host "Authoritative archive: $zipPath"
Write-Host "Hash manifest: $hashManifest"
