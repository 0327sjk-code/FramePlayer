[CmdletBinding()]
param(
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$Version,

    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string]$Repository = '0327sjk-code/FramePlayer',

    [string]$CommitMessage,

    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [switch]$AllowFailure,
        [switch]$EchoOutput
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Command @Arguments 2>&1 | ForEach-Object {
            $line = [string]$_
            [void]$lines.Add($line)
            if ($EchoOutput) {
                Write-Host $line
            }
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $result = [pscustomobject]@{
        ExitCode = $exitCode
        Output = $lines.ToArray()
    }
    if (-not $AllowFailure -and $exitCode -ne 0) {
        $details = ($result.Output -join [Environment]::NewLine).Trim()
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = 'No diagnostic output was returned.'
        }
        throw "Command failed with exit code ${exitCode}: $Command $($Arguments -join ' ')`n$details"
    }
    return $result
}

function Get-RequiredSingleLine {
    param(
        [Parameter(Mandatory = $true)]
        [psobject]$Result,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $value = ($Result.Output -join [Environment]::NewLine).Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Description is missing."
    }
    return $value
}

function Get-NormalizedGitHubRepository {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RemoteUrl
    )

    $url = $RemoteUrl.Trim()
    $path = $null
    if ($url -match '^https://github\.com/(?<path>[^?#]+)$') {
        $path = $Matches.path
    }
    elseif ($url -match '^git@github\.com:(?<path>.+)$') {
        $path = $Matches.path
    }
    elseif ($url -match '^ssh://git@github\.com/(?<path>.+)$') {
        $path = $Matches.path
    }
    else {
        return $null
    }

    $path = $path.Trim('/')
    if ($path.EndsWith('.git', [StringComparison]::OrdinalIgnoreCase)) {
        $path = $path.Substring(0, $path.Length - 4)
    }
    if ($path -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
        return $null
    }
    return $path
}

function Resolve-GitHubCli {
    $command = Get-Command gh.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $packageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (-not [System.IO.Directory]::Exists($packageRoot)) {
        return $null
    }
    $candidate = Get-ChildItem -LiteralPath $packageRoot -Directory |
        Where-Object { $_.Name -like 'GitHub.cli_*' } |
        Sort-Object LastWriteTimeUtc -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\gh.exe' } |
        Where-Object { [System.IO.File]::Exists($_) } |
        Select-Object -First 1
    return $candidate
}

function Get-HttpStatusCode {
    param(
        [Parameter(Mandatory = $true)]
        [System.Exception]$Exception
    )

    $responseProperty = $Exception.PSObject.Properties['Response']
    if ($null -eq $responseProperty -or $null -eq $responseProperty.Value) {
        return 0
    }
    $statusProperty = $responseProperty.Value.PSObject.Properties['StatusCode']
    if ($null -eq $statusProperty -or $null -eq $statusProperty.Value) {
        return 0
    }
    return [int]$statusProperty.Value
}

function Invoke-GitHubRest {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Get', 'Post', 'Patch')]
        [string]$Method,

        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [Parameter(Mandatory = $true)]
        [hashtable]$Headers,

        [string]$Body,
        [string]$InFile,
        [string]$ContentType = 'application/json',
        [switch]$AllowNotFound
    )

    try {
        $parameters = @{
            Method = $Method
            Uri = $Uri
            Headers = $Headers
            ErrorAction = 'Stop'
        }
        if (-not [string]::IsNullOrWhiteSpace($Body)) {
            $parameters.Body = $Body
            $parameters.ContentType = $ContentType
        }
        if (-not [string]::IsNullOrWhiteSpace($InFile)) {
            $parameters.InFile = $InFile
            $parameters.ContentType = $ContentType
        }
        return Invoke-RestMethod @parameters
    }
    catch {
        $statusCode = Get-HttpStatusCode -Exception $_.Exception
        if ($AllowNotFound -and $statusCode -eq 404) {
            return $null
        }
        throw "GitHub API request failed with HTTP status ${statusCode}: $Method $Uri"
    }
}

function Assert-OnlyExpectedGitChanges {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$StatusLines,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedPaths
    )

    foreach ($line in $StatusLines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line.Length -lt 4) {
            throw "Unexpected git status output: $line"
        }
        $path = $line.Substring(3).Trim('"')
        if ($path.Contains(' -> ')) {
            $path = ($path -split ' -> ')[-1].Trim('"')
        }
        $path = $path.Replace('\', '/')
        if ($ExpectedPaths -notcontains $path) {
            throw "Unexpected file changed during release preparation: $path"
        }
    }
}

function Get-CurrentProductVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProductInfoPath
    )

    if (-not [System.IO.File]::Exists($ProductInfoPath)) {
        throw "Product version file was not found: $ProductInfoPath"
    }
    $text = [System.IO.File]::ReadAllText($ProductInfoPath)
    $match = [regex]::Match(
        $text,
        'inline constexpr wchar_t Version\[\][ \t]*=[ \t]*L"(?<major>[0-9]+)\.(?<minor>[0-9]+)\.(?<patch>[0-9]+)";'
    )
    if (-not $match.Success) {
        throw 'Current product version could not be read from ProductInfo.h.'
    }
    return '{0}.{1}.{2}' -f
        $match.Groups['major'].Value,
        $match.Groups['minor'].Value,
        $match.Groups['patch'].Value
}

function Compare-ThreePartVersions {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Left,

        [Parameter(Mandatory = $true)]
        [string]$Right
    )

    $leftParts = $Left.Split('.')
    $rightParts = $Right.Split('.')
    if ($leftParts.Count -ne 3 -or $rightParts.Count -ne 3) {
        throw 'Semantic version comparison requires exactly three components.'
    }
    for ($index = 0; $index -lt 3; ++$index) {
        $leftValue = [int]$leftParts[$index]
        $rightValue = [int]$rightParts[$index]
        if ($leftValue -lt $rightValue) { return -1 }
        if ($leftValue -gt $rightValue) { return 1 }
    }
    return 0
}

function Assert-ProductUpdateConfiguration {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProductInfoPath,

        [Parameter(Mandatory = $true)]
        [string]$Repository
    )

    $text = [System.IO.File]::ReadAllText($ProductInfoPath)
    $repositoryMatch = [regex]::Match(
        $text,
        'inline constexpr wchar_t GitHubRepository\[\][ \t]*=[ \t]*L"(?<value>[^"]+)";'
    )
    if (-not $repositoryMatch.Success -or
        -not $repositoryMatch.Groups['value'].Value.Equals(
            $Repository,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ProductInfo.h GitHubRepository does not match the publication repository.'
    }

    $expectedVersionUrl = "https://github.com/$Repository/releases/latest/download/version.txt"
    $expectedBaseUrl = "https://github.com/$Repository/releases/download/"
    $versionUrlPattern = [regex]::Escape($expectedVersionUrl)
    $baseUrlPattern = [regex]::Escape($expectedBaseUrl)
    if ($text -notmatch ('L"' + $versionUrlPattern + '";') -or
        $text -notmatch ('L"' + $baseUrlPattern + '";')) {
        throw 'ProductInfo.h update URLs do not match the publication repository.'
    }
}

function Assert-VersionState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion
    )

    $escapedVersion = [regex]::Escape($ExpectedVersion)
    $fourPartVersion = "$ExpectedVersion.0"
    $escapedFourPartVersion = [regex]::Escape($fourPartVersion)
    $commaVersion = ($ExpectedVersion -replace '\.', ',') + ',0'
    $escapedCommaVersion = [regex]::Escape($commaVersion)
    $checks = @(
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'src\App\ProductInfo.h'
            Pattern = '^(inline constexpr wchar_t Version\[\][ \t]*=[ \t]*L")' + $escapedVersion + '(";[ \t]*)$'
            Description = 'ProductInfo.h product version'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'CMakeLists.txt'
            Pattern = '^([ \t]*VERSION[ \t]+)' + $escapedVersion + '([ \t]*)$'
            Description = 'CMakeLists.txt project version'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'resources\version.rc'
            Pattern = '^([ \t]*FILEVERSION[ \t]+)' + $escapedCommaVersion + '([ \t]*)$'
            Description = 'version.rc FILEVERSION'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'resources\version.rc'
            Pattern = '^([ \t]*PRODUCTVERSION[ \t]+)' + $escapedCommaVersion + '([ \t]*)$'
            Description = 'version.rc PRODUCTVERSION'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'resources\version.rc'
            Pattern = '^([ \t]*VALUE[ \t]+"FileVersion",[ \t]+")' + $escapedFourPartVersion + '(\\0"[ \t]*)$'
            Description = 'version.rc FileVersion string'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'resources\version.rc'
            Pattern = '^([ \t]*VALUE[ \t]+"ProductVersion",[ \t]+")' + $escapedFourPartVersion + '(\\0"[ \t]*)$'
            Description = 'version.rc ProductVersion string'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'src\App\app.manifest'
            Pattern = '^([ \t]*version=")' + $escapedFourPartVersion + '("[ \t]*/>[ \t]*)$'
            Description = 'app.manifest application identity version'
        },
        [pscustomobject]@{
            Path = Join-Path $ProjectRoot 'version.txt'
            Pattern = '\A' + $escapedVersion + '(\r?\n)?\z'
            Description = 'version.txt product version'
        }
    )

    $options = [System.Text.RegularExpressions.RegexOptions]::Multiline -bor
        [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
    foreach ($check in $checks) {
        if (-not [System.IO.File]::Exists($check.Path)) {
            throw "Required version file was not found: $($check.Path)"
        }
        $text = [System.IO.File]::ReadAllText($check.Path)
        $matches = [regex]::Matches($text, $check.Pattern, $options)
        if ($matches.Count -ne 1) {
            throw "$($check.Description) does not match expected version $ExpectedVersion exactly once."
        }
    }
    $strictVersion = Get-VersionValue `
        -Path (Join-Path $ProjectRoot 'version.txt')
    if ($strictVersion -cne $ExpectedVersion) {
        throw 'version.txt must be strict ASCII without a BOM and match the requested version.'
    }
}

function Assert-ReleaseCompilerPolicy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    $compilerOptionsPath = Join-Path $ProjectRoot 'cmake\CompilerOptions.cmake'
    $cmakePath = Join-Path $ProjectRoot 'CMakeLists.txt'
    foreach ($requiredPath in @($compilerOptionsPath, $cmakePath)) {
        if (-not [System.IO.File]::Exists($requiredPath)) {
            throw "Required CMake policy file was not found: $requiredPath"
        }
    }
    $compilerOptions = [System.IO.File]::ReadAllText($compilerOptionsPath)
    $cmake = [System.IO.File]::ReadAllText($cmakePath)
    if ($compilerOptions -notmatch '(?m)^[ \t]*/W4[ \t]*$') {
        throw 'Project compiler policy must enable /W4.'
    }
    if ($cmake -notmatch '(?m)^[ \t]*target_compile_options\(ZTSequencePlayer PRIVATE /WX\)[ \t]*$') {
        throw 'ZTSequencePlayer Release policy must enable /WX.'
    }
}

function Write-Utf8NoBomAtomically {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $directory = Split-Path -Parent $Path
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    $transactionId = [Guid]::NewGuid().ToString('N')
    $temporaryPath = "$Path.$transactionId.tmp"
    $backupPath = "$Path.$transactionId.bak"
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    try {
        [System.IO.File]::WriteAllText($temporaryPath, $Text, $utf8)
        if ([System.IO.File]::Exists($Path)) {
            [System.IO.File]::Replace($temporaryPath, $Path, $backupPath, $true)
            if ([System.IO.File]::Exists($backupPath)) {
                [System.IO.File]::Delete($backupPath)
            }
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

function Get-FileSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "Cannot hash a missing file: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-StrictAsciiEnvelopeText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "ASCII release asset was not found: $Path"
    }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0) {
        throw "ASCII release asset is empty: $Path"
    }
    foreach ($byte in $bytes) {
        if ($byte -eq 0 -or $byte -gt 0x7F) {
            throw "ASCII release asset contains a BOM or non-ASCII byte: $Path"
        }
    }
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Trim([char[]]@(32, 9, 13, 10))
}

function Get-VersionValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $text = Get-StrictAsciiEnvelopeText -Path $Path
    if ($text -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "Version asset has an invalid format: $Path"
    }
    return $text
}

function Write-ChecksumFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $hash = Get-FileSha256 -Path $ExecutablePath
    Write-Utf8NoBomAtomically `
        -Path $Path `
        -Text ($hash + "`r`n")
    $verifiedHash = Get-ChecksumValue -Path $Path
    if ($verifiedHash -ne $hash) {
        throw 'FramePlayer.exe.sha256 verification failed after writing.'
    }
}

function Get-ChecksumValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "Checksum file was not found: $Path"
    }
    $text = Get-StrictAsciiEnvelopeText -Path $Path
    $match = [regex]::Match(
        $text,
        '^(?<hash>[A-Fa-f0-9]{64})$'
    )
    if (-not $match.Success) {
        throw "Checksum file has an invalid format: $Path"
    }
    return $match.Groups['hash'].Value.ToUpperInvariant()
}

function Assert-ExecutableVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion
    )

    if (-not [System.IO.File]::Exists($Path)) {
        throw "Release executable was not found: $Path"
    }
    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    $actualFileVersion = '{0}.{1}.{2}.{3}' -f
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart
    $expectedFileVersion = "$ExpectedVersion.0"
    if ($actualFileVersion -ne $expectedFileVersion) {
        throw "Built FileVersion mismatch. Expected $expectedFileVersion, got $actualFileVersion."
    }
}

function Get-RemoteTagCommit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Git,

        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot,

        [Parameter(Mandatory = $true)]
        [string]$TagName
    )

    $result = Invoke-NativeCommand `
        -Command $Git `
        -Arguments @(
            '-C', $ProjectRoot,
            'ls-remote', '--tags', 'origin',
            "refs/tags/$TagName",
            "refs/tags/$TagName^{}"
        )
    if ($result.Output.Count -eq 0) {
        return $null
    }
    $baseCommit = $null
    $peeledCommit = $null
    foreach ($line in $result.Output) {
        $parts = $line -split '[ \t]+'
        if ($parts.Count -lt 2) {
            throw "Unexpected ls-remote tag output: $line"
        }
        if ($parts[1] -eq "refs/tags/$TagName^{}") {
            $peeledCommit = $parts[0]
        }
        elseif ($parts[1] -eq "refs/tags/$TagName") {
            $baseCommit = $parts[0]
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($peeledCommit)) {
        return $peeledCommit
    }
    return $baseCommit
}

function Get-GitHubRelease {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [hashtable]$Headers
    )

    if ($UseGitHubCli) {
        $result = Invoke-NativeCommand `
            -Command $GitHubCli `
            -Arguments @(
                'release', 'view', $TagName,
                '--repo', $Repository,
                '--json', 'databaseId,url,tagName,isDraft,isPrerelease,assets'
            ) `
            -AllowFailure
        if ($result.ExitCode -ne 0) {
            $details = ($result.Output -join ' ')
            if ($details -match '(?i)(release not found|HTTP 404|not found)') {
                return $null
            }
            throw "Unable to query GitHub Release ${TagName}: $details"
        }
        $raw = ($result.Output -join '') | ConvertFrom-Json
        return [pscustomobject]@{
            Id = [int64]$raw.databaseId
            TagName = [string]$raw.tagName
            Url = [string]$raw.url
            UploadUrl = $null
            IsDraft = [bool]$raw.isDraft
            IsPrerelease = [bool]$raw.isPrerelease
            Assets = @($raw.assets)
        }
    }

    $raw = Invoke-GitHubRest `
        -Method Get `
        -Uri "https://api.github.com/repos/$Repository/releases/tags/$TagName" `
        -Headers $Headers `
        -AllowNotFound
    if ($null -eq $raw) {
        return $null
    }
    return [pscustomobject]@{
        Id = [int64]$raw.id
        TagName = [string]$raw.tag_name
        Url = [string]$raw.html_url
        UploadUrl = (([string]$raw.upload_url) -replace '\{.*$', '')
        IsDraft = [bool]$raw.draft
        IsPrerelease = [bool]$raw.prerelease
        Assets = @($raw.assets)
    }
}

function Assert-FixedReleaseAssetNames {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [AllowEmptyCollection()]
        [object[]]$Assets,

        [Parameter(Mandatory = $true)]
        [string[]]$AllowedNames,

        [switch]$RequireComplete
    )

    $names = @($Assets | ForEach-Object { [string]$_.name })
    if (@($names | Select-Object -Unique).Count -ne $names.Count) {
        throw 'GitHub Release contains duplicate asset names.'
    }
    foreach ($name in $names) {
        if ($AllowedNames -cnotcontains $name) {
            throw "GitHub Release contains an unexpected asset: $name"
        }
    }
    if ($RequireComplete) {
        foreach ($requiredName in $AllowedNames) {
            if ($names -cnotcontains $requiredName) {
                throw "GitHub Release is missing required asset: $requiredName"
            }
        }
        if ($names.Count -ne $AllowedNames.Count) {
            throw 'GitHub Release does not contain exactly the three fixed assets.'
        }
    }
}

function Download-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [Parameter(Mandatory = $true)]
        [psobject]$Release,

        [Parameter(Mandatory = $true)]
        [string]$AssetName,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory,

        [hashtable]$Headers
    )

    $matchingAssets = @($Release.Assets | Where-Object { [string]$_.name -ceq $AssetName })
    if ($matchingAssets.Count -ne 1) {
        throw "Expected exactly one GitHub asset named $AssetName; found $($matchingAssets.Count)."
    }
    if (-not [System.IO.Directory]::Exists($DestinationDirectory)) {
        [void][System.IO.Directory]::CreateDirectory($DestinationDirectory)
    }
    $destinationPath = Join-Path $DestinationDirectory $AssetName
    if ([System.IO.File]::Exists($destinationPath)) {
        [System.IO.File]::Delete($destinationPath)
    }

    if ($UseGitHubCli) {
        Invoke-NativeCommand `
            -Command $GitHubCli `
            -Arguments @(
                'release', 'download', $TagName,
                '--repo', $Repository,
                '--pattern', $AssetName,
                '--dir', $DestinationDirectory
            ) | Out-Null
    }
    else {
        $assetUrl = [string]$matchingAssets[0].url
        if ([string]::IsNullOrWhiteSpace($assetUrl)) {
            throw "GitHub REST asset URL is missing for $AssetName."
        }
        $downloadHeaders = @{}
        foreach ($key in $Headers.Keys) {
            $downloadHeaders[$key] = $Headers[$key]
        }
        $downloadHeaders.Accept = 'application/octet-stream'
        Invoke-WebRequest `
            -UseBasicParsing `
            -Uri $assetUrl `
            -Headers $downloadHeaders `
            -OutFile $destinationPath `
            -ErrorAction Stop | Out-Null
    }

    if (-not [System.IO.File]::Exists($destinationPath)) {
        throw "GitHub asset download did not produce a file: $AssetName"
    }
    return $destinationPath
}

function New-GitHubRelease {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string]$CommitMessage,

        [Parameter(Mandatory = $true)]
        [object[]]$Assets,

        [hashtable]$Headers
    )

    if ($UseGitHubCli) {
        $arguments = New-Object System.Collections.Generic.List[string]
        foreach ($prefixArgument in @('release', 'create', $TagName)) {
            [void]$arguments.Add($prefixArgument)
        }
        foreach ($asset in $Assets) {
            [void]$arguments.Add([string]$asset.Path)
        }
        foreach ($suffixArgument in @(
            '--repo', $Repository,
            '--title', "FramePlayer $Version",
            '--notes', $CommitMessage,
            '--draft',
            '--verify-tag'
        )) {
            [void]$arguments.Add($suffixArgument)
        }
        Invoke-NativeCommand `
            -Command $GitHubCli `
            -Arguments $arguments.ToArray() `
            -EchoOutput | Out-Null
        return
    }

    $body = @{
        tag_name = $TagName
        name = "FramePlayer $Version"
        body = $CommitMessage
        draft = $true
        prerelease = $false
    } | ConvertTo-Json -Depth 4 -Compress
    $release = Invoke-GitHubRest `
        -Method Post `
        -Uri "https://api.github.com/repos/$Repository/releases" `
        -Headers $Headers `
        -Body $body
    $uploadBase = ([string]$release.upload_url) -replace '\{.*$', ''
    foreach ($asset in $Assets) {
        $escapedName = [Uri]::EscapeDataString([string]$asset.Name)
        Invoke-GitHubRest `
            -Method Post `
            -Uri "${uploadBase}?name=$escapedName" `
            -Headers $Headers `
            -InFile ([string]$asset.Path) `
            -ContentType ([string]$asset.ContentType) | Out-Null
    }
}

function Publish-GitHubRelease {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [Parameter(Mandatory = $true)]
        [psobject]$Release,

        [hashtable]$Headers
    )

    if (-not $Release.IsDraft) {
        return
    }
    if ($UseGitHubCli) {
        Invoke-NativeCommand `
            -Command $GitHubCli `
            -Arguments @(
                'release', 'edit', $TagName,
                '--repo', $Repository,
                '--draft=false'
            ) `
            -EchoOutput | Out-Null
        return
    }
    if ([int64]$Release.Id -le 0) {
        throw 'GitHub draft Release ID is missing.'
    }
    $body = @{ draft = $false } | ConvertTo-Json -Compress
    Invoke-GitHubRest `
        -Method Patch `
        -Uri "https://api.github.com/repos/$Repository/releases/$($Release.Id)" `
        -Headers $Headers `
        -Body $body | Out-Null
}

function Add-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [Parameter(Mandatory = $true)]
        [psobject]$Release,

        [Parameter(Mandatory = $true)]
        [psobject]$Asset,

        [hashtable]$Headers
    )

    if ($UseGitHubCli) {
        Invoke-NativeCommand `
            -Command $GitHubCli `
            -Arguments @(
                'release', 'upload', $TagName,
                [string]$Asset.Path,
                '--repo', $Repository
            ) `
            -EchoOutput | Out-Null
        return
    }

    if ([string]::IsNullOrWhiteSpace([string]$Release.UploadUrl)) {
        throw 'GitHub Release upload URL is missing.'
    }
    $escapedName = [Uri]::EscapeDataString([string]$Asset.Name)
    Invoke-GitHubRest `
        -Method Post `
        -Uri "$($Release.UploadUrl)?name=$escapedName" `
        -Headers $Headers `
        -InFile ([string]$Asset.Path) `
        -ContentType ([string]$Asset.ContentType) | Out-Null
}

function Assert-PublishedReleaseContent {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$UseGitHubCli,

        [string]$GitHubCli,

        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$TagName,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [psobject]$Release,

        [Parameter(Mandatory = $true)]
        [string]$AuditDirectory,

        [hashtable]$Headers,

        [switch]$AllowDraft
    )

    $fixedAssetNames = @('FramePlayer.exe', 'version.txt', 'FramePlayer.exe.sha256')
    Assert-FixedReleaseAssetNames `
        -Assets $Release.Assets `
        -AllowedNames $fixedAssetNames `
        -RequireComplete
    if ((-not $AllowDraft -and $Release.IsDraft) -or $Release.IsPrerelease) {
        throw 'The fixed FramePlayer release has an invalid publication state.'
    }

    $downloadedExecutable = Download-GitHubReleaseAsset `
        -UseGitHubCli $UseGitHubCli `
        -GitHubCli $GitHubCli `
        -Repository $Repository `
        -TagName $TagName `
        -Release $Release `
        -AssetName 'FramePlayer.exe' `
        -DestinationDirectory $AuditDirectory `
        -Headers $Headers
    $downloadedVersion = Download-GitHubReleaseAsset `
        -UseGitHubCli $UseGitHubCli `
        -GitHubCli $GitHubCli `
        -Repository $Repository `
        -TagName $TagName `
        -Release $Release `
        -AssetName 'version.txt' `
        -DestinationDirectory $AuditDirectory `
        -Headers $Headers
    $downloadedChecksum = Download-GitHubReleaseAsset `
        -UseGitHubCli $UseGitHubCli `
        -GitHubCli $GitHubCli `
        -Repository $Repository `
        -TagName $TagName `
        -Release $Release `
        -AssetName 'FramePlayer.exe.sha256' `
        -DestinationDirectory $AuditDirectory `
        -Headers $Headers

    Assert-ExecutableVersion -Path $downloadedExecutable -ExpectedVersion $Version
    if ((Get-VersionValue -Path $downloadedVersion) -cne $Version) {
        throw 'Published version.txt does not match the requested version.'
    }
    $publishedHash = Get-ChecksumValue -Path $downloadedChecksum
    $actualHash = Get-FileSha256 -Path $downloadedExecutable
    if ($publishedHash -ne $actualHash) {
        throw 'Published FramePlayer.exe.sha256 does not match the published executable.'
    }
}

function Invoke-AnonymousDownloadWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Uri,

        [Parameter(Mandatory = $true)]
        [string]$Destination,

        [int]$MaximumAttempts = 8
    )

    $lastFailure = $null
    for ($attempt = 1; $attempt -le $MaximumAttempts; ++$attempt) {
        try {
            if ([System.IO.File]::Exists($Destination)) {
                [System.IO.File]::Delete($Destination)
            }
            Invoke-WebRequest `
                -UseBasicParsing `
                -Uri $Uri `
                -OutFile $Destination `
                -MaximumRedirection 10 `
                -ErrorAction Stop | Out-Null
            if ([System.IO.File]::Exists($Destination) -and
                (Get-Item -LiteralPath $Destination).Length -gt 0) {
                return
            }
            $lastFailure = 'download produced an empty file'
        }
        catch {
            $lastFailure = $_.Exception.Message
        }
        if ($attempt -lt $MaximumAttempts) {
            Start-Sleep -Seconds 2
        }
    }
    throw "Anonymous download failed after $MaximumAttempts attempts: $Uri ($lastFailure)"
}

function Assert-AnonymousClientEndpoints {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Repository,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string]$AuditDirectory
    )

    [void][System.IO.Directory]::CreateDirectory($AuditDirectory)
    $tagName = "v$Version"
    $latestBase = "https://github.com/$Repository/releases/latest/download"
    $versionBase = "https://github.com/$Repository/releases/download/$tagName"
    $latestVersion = Join-Path $AuditDirectory 'latest-version.txt'
    $latestExecutable = Join-Path $AuditDirectory 'latest-FramePlayer.exe'
    $latestChecksum = Join-Path $AuditDirectory 'latest-FramePlayer.exe.sha256'
    $versionedExecutable = Join-Path $AuditDirectory 'versioned-FramePlayer.exe'
    $versionedChecksum = Join-Path $AuditDirectory 'versioned-FramePlayer.exe.sha256'

    Invoke-AnonymousDownloadWithRetry `
        -Uri "$latestBase/version.txt" `
        -Destination $latestVersion
    Invoke-AnonymousDownloadWithRetry `
        -Uri "$latestBase/FramePlayer.exe" `
        -Destination $latestExecutable
    Invoke-AnonymousDownloadWithRetry `
        -Uri "$latestBase/FramePlayer.exe.sha256" `
        -Destination $latestChecksum
    Invoke-AnonymousDownloadWithRetry `
        -Uri "$versionBase/FramePlayer.exe" `
        -Destination $versionedExecutable
    Invoke-AnonymousDownloadWithRetry `
        -Uri "$versionBase/FramePlayer.exe.sha256" `
        -Destination $versionedChecksum

    if ((Get-VersionValue -Path $latestVersion) -cne $Version) {
        throw 'The anonymous latest version endpoint does not expose the published version.'
    }
    Assert-ExecutableVersion -Path $latestExecutable -ExpectedVersion $Version
    Assert-ExecutableVersion -Path $versionedExecutable -ExpectedVersion $Version
    $latestHash = Get-FileSha256 -Path $latestExecutable
    $versionedHash = Get-FileSha256 -Path $versionedExecutable
    if ($latestHash -ne $versionedHash -or
        (Get-ChecksumValue -Path $latestChecksum) -ne $latestHash -or
        (Get-ChecksumValue -Path $versionedChecksum) -ne $versionedHash) {
        throw 'Anonymous latest and version-specific update endpoints are not byte-consistent.'
    }
}

$projectRoot = [System.IO.Path]::GetFullPath(
    (Split-Path -Parent $PSScriptRoot)
)
$productInfoPath = Join-Path $projectRoot 'src\App\ProductInfo.h'
$setVersionScript = Join-Path $PSScriptRoot 'set-version.ps1'
$buildScript = Join-Path $projectRoot 'scripts\build.ps1'
$releaseExecutable = Join-Path $projectRoot 'build\bin\Release\FramePlayer.exe'
$versionFile = Join-Path $projectRoot 'version.txt'
$checksumFile = Join-Path $projectRoot 'build\bin\Release\FramePlayer.exe.sha256'
$expectedReleasePaths = @(
    'src/App/ProductInfo.h',
    'CMakeLists.txt',
    'resources/version.rc',
    'src/App/app.manifest',
    'version.txt'
)
$fixedAssetNames = @('FramePlayer.exe', 'version.txt', 'FramePlayer.exe.sha256')

$currentVersion = Get-CurrentProductVersion -ProductInfoPath $productInfoPath
Assert-ProductUpdateConfiguration `
    -ProductInfoPath $productInfoPath `
    -Repository $Repository
if ([string]::IsNullOrWhiteSpace($Version)) {
    if ($Resume) {
        $Version = $currentVersion
        Write-Host "No Version was supplied; resuming the current version $Version."
    }
    else {
        $parts = $currentVersion.Split('.')
        $major = [int]$parts[0]
        $minor = [int]$parts[1]
        $patch = [int]$parts[2]
        if ($major -gt 65535 -or $minor -gt 65535 -or $patch -ge 65535) {
            throw 'Current product version cannot be automatically patch-incremented.'
        }
        $Version = '{0}.{1}.{2}' -f $major, $minor, ($patch + 1)
        Write-Host "No Version was supplied; publishing the next patch version $Version."
    }
}

$versionComponents = $Version.Split('.')
foreach ($component in $versionComponents) {
    $numericComponent = 0
    if (-not [int]::TryParse($component, [ref]$numericComponent) -or
        $numericComponent -lt 0 -or
        $numericComponent -gt 65535) {
        throw "Each Windows version component must be between 0 and 65535: $component"
    }
}
if (-not $Resume -and
    (Compare-ThreePartVersions -Left $Version -Right $currentVersion) -le 0) {
    throw "A new Release version must be greater than the current source version $currentVersion."
}

$tagName = "v$Version"
if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    $CommitMessage = "Release FramePlayer $tagName"
}
else {
    $CommitMessage = $CommitMessage.Trim()
    if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
        throw 'CommitMessage cannot contain only whitespace.'
    }
}

foreach ($requiredFile in @($setVersionScript, $buildScript, $productInfoPath)) {
    if (-not [System.IO.File]::Exists($requiredFile)) {
        throw "Required release file was not found: $requiredFile"
    }
}

$gitCommandInfo = Get-Command git.exe -ErrorAction SilentlyContinue
if ($null -eq $gitCommandInfo) {
    throw 'git.exe was not found in PATH.'
}
$git = $gitCommandInfo.Source

$gitRootResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'rev-parse', '--show-toplevel')
$gitRoot = [System.IO.Path]::GetFullPath(
    (Get-RequiredSingleLine -Result $gitRootResult -Description 'Git repository root')
).TrimEnd('\')
if (-not $gitRoot.Equals(
        $projectRoot.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Project must be the Git repository root. Expected: $projectRoot Actual: $gitRoot"
}

$identityName = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'config', '--get', 'user.name') `
    -AllowFailure
$identityEmail = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'config', '--get', 'user.email') `
    -AllowFailure
if ($identityName.ExitCode -ne 0 -or
    [string]::IsNullOrWhiteSpace(($identityName.Output -join '').Trim()) -or
    $identityEmail.ExitCode -ne 0 -or
    [string]::IsNullOrWhiteSpace(($identityEmail.Output -join '').Trim())) {
    throw 'Git identity is incomplete. Configure user.name and user.email before publishing.'
}

$branchResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'symbolic-ref', '--quiet', '--short', 'HEAD')
$branchName = Get-RequiredSingleLine -Result $branchResult -Description 'Current Git branch'

$statusBefore = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'status', '--porcelain=v1', '--untracked-files=all')
if ($statusBefore.Output.Count -ne 0) {
    throw "Git working tree must be clean before publishing:`n$($statusBefore.Output -join [Environment]::NewLine)"
}

$originResult = Invoke-NativeCommand `
    -Command $git `
    -Arguments @('-C', $projectRoot, 'remote', 'get-url', 'origin')
$originUrl = Get-RequiredSingleLine -Result $originResult -Description 'Git origin remote'
$originRepository = Get-NormalizedGitHubRepository -RemoteUrl $originUrl
if ($null -eq $originRepository -or
    -not $originRepository.Equals($Repository, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The origin remote does not match the requested GitHub Repository parameter.'
}

$previousGitTerminalPrompt = $env:GIT_TERMINAL_PROMPT
$env:GIT_TERMINAL_PROMPT = '0'
$releasePreparationStarted = $false
$releaseCommitCreated = $false
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'FramePlayer-publish-' + [Guid]::NewGuid().ToString('N')
)
try {
    Write-Host 'Checking Git remote access and synchronization...'
    Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'fetch', '--prune', '--tags', 'origin') `
        -EchoOutput | Out-Null

    $headCommit = Get-RequiredSingleLine `
        -Result (Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'rev-parse', 'HEAD')) `
        -Description 'Current Git commit'
    $remoteBranch = Invoke-NativeCommand `
        -Command $git `
        -Arguments @(
            '-C', $projectRoot,
            'ls-remote', '--heads', 'origin', "refs/heads/$branchName"
        )
    $remoteBranchCommit = $null
    if ($remoteBranch.Output.Count -gt 0) {
        $remoteBranchCommit = (($remoteBranch.Output[0] -split '[ \t]+')[0]).Trim()
    }

    $gh = Resolve-GitHubCli
    $useGitHubCli = -not [string]::IsNullOrWhiteSpace($gh)
    $githubHeaders = $null
    if ($useGitHubCli) {
        Write-Host 'Using GitHub CLI for release publication.'
        Invoke-NativeCommand `
            -Command $gh `
            -Arguments @('auth', 'status', '--hostname', 'github.com') `
            -EchoOutput | Out-Null
        $repositoryResult = Invoke-NativeCommand `
            -Command $gh `
            -Arguments @('repo', 'view', $Repository, '--json', 'nameWithOwner,visibility')
        $repositoryView = ($repositoryResult.Output -join '') | ConvertFrom-Json
        if (-not ([string]$repositoryView.nameWithOwner).Equals(
                $Repository,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'GitHub CLI authenticated repository does not match Repository.'
        }
        if (-not ([string]$repositoryView.visibility).Equals(
                'PUBLIC',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'FramePlayer update publication requires a PUBLIC GitHub repository.'
        }
    }
    else {
        if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
            throw 'gh.exe is unavailable and GITHUB_TOKEN is not set.'
        }
        Write-Host 'GitHub CLI was not found; using GitHub REST API.'
        $githubHeaders = @{
            Authorization = "Bearer $($env:GITHUB_TOKEN)"
            Accept = 'application/vnd.github+json'
            'X-GitHub-Api-Version' = '2022-11-28'
            'User-Agent' = 'FramePlayer-Publisher'
        }
        $repositoryView = Invoke-GitHubRest `
            -Method Get `
            -Uri "https://api.github.com/repos/$Repository" `
            -Headers $githubHeaders
        if (-not ([string]$repositoryView.full_name).Equals(
                $Repository,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'GITHUB_TOKEN cannot access the requested Repository.'
        }
        if ([bool]$repositoryView.private -or
            -not ([string]$repositoryView.visibility).Equals(
                'public',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'FramePlayer update publication requires a PUBLIC GitHub repository.'
        }
    }

    $localTag = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'show-ref', '--verify', '--quiet', "refs/tags/$tagName") `
        -AllowFailure
    if ($localTag.ExitCode -ne 0 -and $localTag.ExitCode -ne 1) {
        throw "Unable to check local tag: $tagName"
    }
    $remoteTagCommit = Get-RemoteTagCommit `
        -Git $git `
        -ProjectRoot $projectRoot `
        -TagName $tagName
    $existingRelease = Get-GitHubRelease `
        -UseGitHubCli $useGitHubCli `
        -GitHubCli $gh `
        -Repository $Repository `
        -TagName $tagName `
        -Headers $githubHeaders

    if (-not $Resume) {
        if (-not [string]::IsNullOrWhiteSpace($remoteBranchCommit) -and
            -not $remoteBranchCommit.Equals($headCommit, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Local branch $branchName is not exactly synchronized with origin/$branchName."
        }
        if ($localTag.ExitCode -eq 0) {
            throw "Local tag already exists: $tagName. Use -Resume only to safely complete an interrupted publication."
        }
        if (-not [string]::IsNullOrWhiteSpace($remoteTagCommit)) {
            throw "Remote tag already exists: $tagName. Use -Resume only to safely complete an interrupted publication."
        }
        if ($null -ne $existingRelease) {
            throw "GitHub Release already exists: $tagName. Use -Resume only to safely complete it."
        }

        Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'push', '--dry-run', 'origin', "HEAD:refs/heads/$branchName") `
            -EchoOutput | Out-Null

        Assert-ReleaseCompilerPolicy -ProjectRoot $projectRoot
        Write-Host "Updating version to $Version..."
        $releasePreparationStarted = $true
        Invoke-NativeCommand `
            -Command (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
            -Arguments @(
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy', 'Bypass',
                '-File', $setVersionScript,
                '-Version', $Version
            ) `
            -EchoOutput | Out-Null

        Write-Host 'Rebuilding Release|x64 with /W4 /WX and running CTest...'
        Invoke-NativeCommand `
            -Command (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
            -Arguments @(
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy', 'Bypass',
                '-File', $buildScript,
                '-Configuration', 'Release',
                '-Clean'
            ) `
            -EchoOutput | Out-Null

        Assert-VersionState -ProjectRoot $projectRoot -ExpectedVersion $Version
        Assert-ExecutableVersion -Path $releaseExecutable -ExpectedVersion $Version
        Write-ChecksumFile -Path $checksumFile -ExecutablePath $releaseExecutable

        $statusAfter = Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'status', '--porcelain=v1', '--untracked-files=all')
        Assert-OnlyExpectedGitChanges `
            -StatusLines $statusAfter.Output `
            -ExpectedPaths $expectedReleasePaths
        if ($statusAfter.Output.Count -eq 0) {
            throw 'Release preparation produced no Git changes; use a new version number.'
        }

        Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'diff', '--check') | Out-Null
        $addArguments = @('-C', $projectRoot, 'add', '--') + $expectedReleasePaths
        Invoke-NativeCommand -Command $git -Arguments $addArguments | Out-Null
        $stagedFiles = Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'diff', '--cached', '--name-only')
        if ($stagedFiles.Output.Count -eq 0) {
            throw 'No release files were staged for commit.'
        }
        Assert-OnlyExpectedGitChanges `
            -StatusLines @($stagedFiles.Output | ForEach-Object { "M  $_" }) `
            -ExpectedPaths $expectedReleasePaths

        Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'commit', '-m', $CommitMessage) `
            -EchoOutput | Out-Null
        $releaseCommitCreated = $true
        Invoke-NativeCommand `
            -Command $git `
            -Arguments @('-C', $projectRoot, 'tag', '-a', $tagName, '-m', $CommitMessage) | Out-Null

        Write-Host "Pushing $branchName and $tagName atomically..."
        Invoke-NativeCommand `
            -Command $git `
            -Arguments @(
                '-C', $projectRoot,
                'push', '--atomic', 'origin',
                "HEAD:refs/heads/$branchName",
                "refs/tags/${tagName}:refs/tags/${tagName}"
            ) `
            -EchoOutput | Out-Null

        $assets = @(
            [pscustomobject]@{
                Name = 'FramePlayer.exe'
                Path = $releaseExecutable
                ContentType = 'application/vnd.microsoft.portable-executable'
            },
            [pscustomobject]@{
                Name = 'version.txt'
                Path = $versionFile
                ContentType = 'text/plain'
            },
            [pscustomobject]@{
                Name = 'FramePlayer.exe.sha256'
                Path = $checksumFile
                ContentType = 'text/plain'
            }
        )
        Write-Host 'Creating GitHub Release and uploading the three fixed assets...'
        New-GitHubRelease `
            -UseGitHubCli $useGitHubCli `
            -GitHubCli $gh `
            -Repository $Repository `
            -TagName $tagName `
            -Version $Version `
            -CommitMessage $CommitMessage `
            -Assets $assets `
            -Headers $githubHeaders
    }
    else {
        if ($localTag.ExitCode -ne 0) {
            throw "Resume requires an existing local or fetched tag: $tagName"
        }
        $tagCommit = Get-RequiredSingleLine `
            -Result (Invoke-NativeCommand `
                -Command $git `
                -Arguments @('-C', $projectRoot, 'rev-list', '-n', '1', "refs/tags/$tagName")) `
            -Description "Commit for tag $tagName"
        if (-not $headCommit.Equals($tagCommit, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Resume requires HEAD to be the exact tagged commit $tagName."
        }
        if (-not [string]::IsNullOrWhiteSpace($remoteTagCommit) -and
            -not $remoteTagCommit.Equals($tagCommit, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Remote tag $tagName does not point to the local tagged commit."
        }
        if (-not [string]::IsNullOrWhiteSpace($remoteBranchCommit) -and
            -not $remoteBranchCommit.Equals($headCommit, [StringComparison]::OrdinalIgnoreCase)) {
            $ancestorResult = Invoke-NativeCommand `
                -Command $git `
                -Arguments @('-C', $projectRoot, 'merge-base', '--is-ancestor', $remoteBranchCommit, $headCommit) `
                -AllowFailure
            if ($ancestorResult.ExitCode -ne 0) {
                throw "origin/$branchName is not a safe fast-forward ancestor of the tagged commit."
            }
        }

        Assert-VersionState -ProjectRoot $projectRoot -ExpectedVersion $Version
        Assert-ReleaseCompilerPolicy -ProjectRoot $projectRoot
        Write-Host "Rebuilding the exact tagged FramePlayer $Version source..."
        Invoke-NativeCommand `
            -Command (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
            -Arguments @(
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy', 'Bypass',
                '-File', $buildScript,
                '-Configuration', 'Release',
                '-Clean'
            ) `
            -EchoOutput | Out-Null
        Assert-ExecutableVersion -Path $releaseExecutable -ExpectedVersion $Version
        Write-ChecksumFile -Path $checksumFile -ExecutablePath $releaseExecutable

        if ([string]::IsNullOrWhiteSpace($remoteTagCommit) -or
            [string]::IsNullOrWhiteSpace($remoteBranchCommit) -or
            -not $remoteBranchCommit.Equals($headCommit, [StringComparison]::OrdinalIgnoreCase)) {
            $pushArguments = New-Object System.Collections.Generic.List[string]
            foreach ($argument in @('-C', $projectRoot, 'push')) {
                [void]$pushArguments.Add($argument)
            }
            if ([string]::IsNullOrWhiteSpace($remoteTagCommit)) {
                [void]$pushArguments.Add('--atomic')
            }
            [void]$pushArguments.Add('origin')
            [void]$pushArguments.Add("HEAD:refs/heads/$branchName")
            if ([string]::IsNullOrWhiteSpace($remoteTagCommit)) {
                [void]$pushArguments.Add("refs/tags/${tagName}:refs/tags/${tagName}")
            }
            $dryRunArguments = New-Object System.Collections.Generic.List[string]
            foreach ($argument in $pushArguments) {
                [void]$dryRunArguments.Add($argument)
            }
            $dryRunArguments.Insert(3, '--dry-run')
            Invoke-NativeCommand `
                -Command $git `
                -Arguments $dryRunArguments.ToArray() `
                -EchoOutput | Out-Null
            Write-Host 'Completing the missing remote branch/tag publication...'
            Invoke-NativeCommand `
                -Command $git `
                -Arguments $pushArguments.ToArray() `
                -EchoOutput | Out-Null
        }

        if ($null -eq $existingRelease) {
            $assets = @(
                [pscustomobject]@{
                    Name = 'FramePlayer.exe'
                    Path = $releaseExecutable
                    ContentType = 'application/vnd.microsoft.portable-executable'
                },
                [pscustomobject]@{
                    Name = 'version.txt'
                    Path = $versionFile
                    ContentType = 'text/plain'
                },
                [pscustomobject]@{
                    Name = 'FramePlayer.exe.sha256'
                    Path = $checksumFile
                    ContentType = 'text/plain'
                }
            )
            Write-Host 'The tag exists but the GitHub Release is missing; creating it safely...'
            New-GitHubRelease `
                -UseGitHubCli $useGitHubCli `
                -GitHubCli $gh `
                -Repository $Repository `
                -TagName $tagName `
                -Version $Version `
                -CommitMessage $CommitMessage `
                -Assets $assets `
                -Headers $githubHeaders
        }
        else {
            if ($existingRelease.TagName -cne $tagName) {
                throw 'Existing GitHub Release tag does not match the requested tag.'
            }
            if ($existingRelease.IsPrerelease) {
                throw 'Resume will not mutate a prerelease.'
            }
            Assert-FixedReleaseAssetNames `
                -Assets $existingRelease.Assets `
                -AllowedNames $fixedAssetNames

            $resumeDirectory = Join-Path $temporaryRoot 'resume'
            [void][System.IO.Directory]::CreateDirectory($resumeDirectory)
            $existingNames = @($existingRelease.Assets | ForEach-Object { [string]$_.name })
            $canonicalExecutable = $releaseExecutable
            $canonicalHash = Get-FileSha256 -Path $releaseExecutable

            if ($existingNames -contains 'FramePlayer.exe') {
                $existingExecutable = Download-GitHubReleaseAsset `
                    -UseGitHubCli $useGitHubCli `
                    -GitHubCli $gh `
                    -Repository $Repository `
                    -TagName $tagName `
                    -Release $existingRelease `
                    -AssetName 'FramePlayer.exe' `
                    -DestinationDirectory $resumeDirectory `
                    -Headers $githubHeaders
                Assert-ExecutableVersion -Path $existingExecutable -ExpectedVersion $Version
                if ((Get-FileSha256 -Path $existingExecutable) -ne $canonicalHash) {
                    throw 'Existing GitHub executable does not match the exact trusted tagged build.'
                }
            }
            if ($existingNames -contains 'version.txt') {
                $existingVersionPath = Download-GitHubReleaseAsset `
                    -UseGitHubCli $useGitHubCli `
                    -GitHubCli $gh `
                    -Repository $Repository `
                    -TagName $tagName `
                    -Release $existingRelease `
                    -AssetName 'version.txt' `
                    -DestinationDirectory $resumeDirectory `
                    -Headers $githubHeaders
                if ((Get-VersionValue -Path $existingVersionPath) -cne $Version) {
                    throw 'Existing GitHub version.txt conflicts with the requested version.'
                }
            }

            if ($existingNames -contains 'FramePlayer.exe.sha256') {
                $existingChecksumPath = Download-GitHubReleaseAsset `
                    -UseGitHubCli $useGitHubCli `
                    -GitHubCli $gh `
                    -Repository $Repository `
                    -TagName $tagName `
                    -Release $existingRelease `
                    -AssetName 'FramePlayer.exe.sha256' `
                    -DestinationDirectory $resumeDirectory `
                    -Headers $githubHeaders
                $existingHash = Get-ChecksumValue -Path $existingChecksumPath
                if ($existingHash -ne $canonicalHash) {
                    throw 'Existing GitHub checksum conflicts with the canonical executable; no assets were overwritten.'
                }
            }

            $resumeChecksumPath = Join-Path $resumeDirectory 'FramePlayer.exe.sha256'
            if ($existingNames -notcontains 'FramePlayer.exe.sha256') {
                Write-ChecksumFile `
                    -Path $resumeChecksumPath `
                    -ExecutablePath $canonicalExecutable
            }
            $missingAssets = New-Object System.Collections.Generic.List[object]
            if ($existingNames -notcontains 'FramePlayer.exe') {
                [void]$missingAssets.Add([pscustomobject]@{
                    Name = 'FramePlayer.exe'
                    Path = $releaseExecutable
                    ContentType = 'application/vnd.microsoft.portable-executable'
                })
            }
            if ($existingNames -notcontains 'version.txt') {
                [void]$missingAssets.Add([pscustomobject]@{
                    Name = 'version.txt'
                    Path = $versionFile
                    ContentType = 'text/plain'
                })
            }
            if ($existingNames -notcontains 'FramePlayer.exe.sha256') {
                [void]$missingAssets.Add([pscustomobject]@{
                    Name = 'FramePlayer.exe.sha256'
                    Path = $resumeChecksumPath
                    ContentType = 'text/plain'
                })
            }

            foreach ($asset in $missingAssets) {
                Write-Host "Uploading missing release asset: $($asset.Name)"
                Add-GitHubReleaseAsset `
                    -UseGitHubCli $useGitHubCli `
                    -GitHubCli $gh `
                    -Repository $Repository `
                    -TagName $tagName `
                    -Release $existingRelease `
                    -Asset $asset `
                    -Headers $githubHeaders
            }
            if ($missingAssets.Count -eq 0) {
                Write-Host 'All three fixed release assets already exist and passed validation.'
            }
        }
    }

    $publishedRelease = Get-GitHubRelease `
        -UseGitHubCli $useGitHubCli `
        -GitHubCli $gh `
        -Repository $Repository `
        -TagName $tagName `
        -Headers $githubHeaders
    if ($null -eq $publishedRelease) {
        throw "GitHub Release was not found after publication: $tagName"
    }
    $auditDirectory = Join-Path $temporaryRoot 'final-audit'
    [void][System.IO.Directory]::CreateDirectory($auditDirectory)
    if ($publishedRelease.IsDraft) {
        Write-Host 'Auditing all draft assets before making the Release public...'
        Assert-PublishedReleaseContent `
            -UseGitHubCli $useGitHubCli `
            -GitHubCli $gh `
            -Repository $Repository `
            -TagName $tagName `
            -Version $Version `
            -Release $publishedRelease `
            -AuditDirectory $auditDirectory `
            -Headers $githubHeaders `
            -AllowDraft
        Publish-GitHubRelease `
            -UseGitHubCli $useGitHubCli `
            -GitHubCli $gh `
            -Repository $Repository `
            -TagName $tagName `
            -Release $publishedRelease `
            -Headers $githubHeaders
        $publishedRelease = Get-GitHubRelease `
            -UseGitHubCli $useGitHubCli `
            -GitHubCli $gh `
            -Repository $Repository `
            -TagName $tagName `
            -Headers $githubHeaders
        if ($null -eq $publishedRelease -or $publishedRelease.IsDraft) {
            throw 'GitHub Release did not leave draft state after publication.'
        }
    }
    Assert-PublishedReleaseContent `
        -UseGitHubCli $useGitHubCli `
        -GitHubCli $gh `
        -Repository $Repository `
        -TagName $tagName `
        -Version $Version `
        -Release $publishedRelease `
        -AuditDirectory $auditDirectory `
        -Headers $githubHeaders

    $anonymousAuditDirectory = Join-Path $temporaryRoot 'anonymous-audit'
    Assert-AnonymousClientEndpoints `
        -Repository $Repository `
        -Version $Version `
        -AuditDirectory $anonymousAuditDirectory

    $finalStatus = Invoke-NativeCommand `
        -Command $git `
        -Arguments @('-C', $projectRoot, 'status', '--porcelain=v1', '--untracked-files=all')
    if ($finalStatus.Output.Count -ne 0) {
        throw "Publication completed, but the Git working tree is not clean:`n$($finalStatus.Output -join [Environment]::NewLine)"
    }
    Write-Host "Published FramePlayer $Version successfully: $($publishedRelease.Url)"
}
catch {
    $originalFailure = $_
    if ($releasePreparationStarted -and -not $releaseCommitCreated) {
        try {
            $restoreArguments = @(
                '-C', $projectRoot,
                'restore', '--staged', '--worktree', '--'
            ) + $expectedReleasePaths
            Invoke-NativeCommand `
                -Command $git `
                -Arguments $restoreArguments | Out-Null
        }
        catch {
            throw "Release failed: $($originalFailure.Exception.Message) Automatic source rollback also failed: $($_.Exception.Message)"
        }
    }
    throw $originalFailure
}
finally {
    try {
        if ([System.IO.Directory]::Exists($temporaryRoot)) {
            [System.IO.Directory]::Delete($temporaryRoot, $true)
        }
    }
    catch {
        Write-Warning "Could not remove temporary publication files: $($_.Exception.Message)"
    }
    try {
        if ($null -eq $previousGitTerminalPrompt) {
            Remove-Item Env:GIT_TERMINAL_PROMPT -ErrorAction SilentlyContinue
        }
        else {
            $env:GIT_TERMINAL_PROMPT = $previousGitTerminalPrompt
        }
    }
    catch {
        Write-Warning "Could not restore GIT_TERMINAL_PROMPT: $($_.Exception.Message)"
    }
}
