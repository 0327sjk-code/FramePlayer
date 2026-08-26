[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',

    [switch]$Clean,

    [string]$RealPngPath = $env:ZT_SEQUENCE_TEST_PNG
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'Visual Studio 2022 Build Tools with the MSVC x64 toolchain was not found.'
}

$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake was not found: $cmake"
}

$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) {
    throw "CTest was not found: $ctest"
}

if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) {
    $resolvedRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\')
    $resolvedBuild = [System.IO.Path]::GetFullPath($buildDirectory).TrimEnd('\')
    if (-not $resolvedBuild.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside the project root: $resolvedBuild"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

& $cmake -S $projectRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

& $cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "CTest failed with exit code $LASTEXITCODE"
}

$executable = Join-Path $buildDirectory "bin\$Configuration\FramePlayer.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "The build completed but the executable was not found: $executable"
}

if (-not [string]::IsNullOrWhiteSpace($RealPngPath)) {
    if (-not (Test-Path -LiteralPath $RealPngPath -PathType Leaf)) {
        throw "Real PNG validation file was not found: $RealPngPath"
    }
    $verificationScript = Join-Path $PSScriptRoot 'verify-real-png.ps1'
    & $verificationScript -PngPath $RealPngPath -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "Real PNG validation failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Build completed: $executable"
