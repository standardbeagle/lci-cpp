# lci installer for Windows — downloads the matching prebuilt binary from
# GitHub releases.
#
#   irm https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.ps1 | iex
#
# Environment:
#   LCI_VERSION   pin a specific release (e.g. 0.6.0); default: latest
#   LCI_PREFIX    install directory; default: %LOCALAPPDATA%\Programs\lci
#   LCI_DRYRUN    set to 1 to print the resolved download URL and exit
#   LCI_NO_SLOP   set to 1 to skip registering the lci MCP server with slop-mcp
#   LCI_AUTO_UPDATE  set to 1 to schedule a weekly `lci update` (Task Scheduler)
#
# Contract: see docs/superpowers/specs/2026-06-07-install-update-distribution-design.md
$ErrorActionPreference = 'Stop'

$repo = 'standardbeagle/lci-cpp'

function Assert-GitHubUrl([string]$url) {
    $u = [Uri]$url
    $ok = $u.Scheme -eq 'https' -and (
        $u.Host -in @('github.com', 'api.github.com', 'codeload.github.com', 'githubusercontent.com') -or
        $u.Host.EndsWith('.githubusercontent.com')
    )
    if (-not $ok) { throw "refusing non-GitHub URL: $url" }
}

$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64') {
    throw "unsupported architecture: $arch (only x86_64/AMD64 has a release artifact; build from source)"
}

if ($env:LCI_VERSION) {
    $api = "https://api.github.com/repos/$repo/releases/tags/v$($env:LCI_VERSION)"
} else {
    $api = "https://api.github.com/repos/$repo/releases/latest"
}

$release = Invoke-RestMethod -Uri $api -Headers @{ 'Accept' = 'application/vnd.github+json'; 'User-Agent' = 'lci-installer' }

$asset = $release.assets |
    Where-Object { $_.name -match '\.tar\.gz$' -and ($_.name -match '(?i)win64' -or $_.name -match '(?i)windows') } |
    Select-Object -First 1

if (-not $asset) {
    throw "no Windows release asset (*.tar.gz) found at $api"
}

if ($env:LCI_DRYRUN -eq '1') {
    Write-Output $asset.browser_download_url
    return
}

if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw 'tar is required but was not found on PATH (Windows 10+ ships it as tar.exe)'
}

if ($env:LCI_PREFIX) {
    $prefix = $env:LCI_PREFIX
} else {
    $prefix = Join-Path $env:LOCALAPPDATA 'Programs\lci'
}
New-Item -ItemType Directory -Force -Path $prefix | Out-Null

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("lci-install-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $tarball = Join-Path $tmp $asset.name
    Assert-GitHubUrl $asset.browser_download_url
    Write-Output "Downloading $($asset.browser_download_url)"
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $tarball -Headers @{ 'User-Agent' = 'lci-installer' }

    # Verify integrity against the release SHA256SUMS when present.
    $sums = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS' } | Select-Object -First 1
    if ($sums) {
        Assert-GitHubUrl $sums.browser_download_url
        # Download to a file and read it back rather than using .Content:
        # on Windows PowerShell 5.1, (Invoke-WebRequest -UseBasicParsing).Content
        # is a Byte[], not a string, so -split would operate per-byte.
        $sumsFile = Join-Path $tmp 'SHA256SUMS'
        Invoke-WebRequest -UseBasicParsing -Uri $sums.browser_download_url -OutFile $sumsFile -Headers @{ 'User-Agent' = 'lci-installer' }
        $sumsText = Get-Content -Raw -Path $sumsFile
        $line = ($sumsText -split "`n") | Where-Object { $_ -match "\s$([regex]::Escape($asset.name))\s*$" } | Select-Object -First 1
        if (-not $line) { throw "SHA256SUMS has no entry for $($asset.name)" }
        $expected = ($line -split '\s+')[0].ToLower()
        $actual = (Get-FileHash -Algorithm SHA256 -Path $tarball).Hash.ToLower()
        if ($actual -ne $expected) {
            throw "checksum mismatch for $($asset.name) (expected $expected, got $actual)"
        }
        Write-Output 'Verified checksum.'
    } else {
        Write-Warning 'release has no SHA256SUMS; skipping integrity check'
    }

    tar -xf $tarball -C $tmp
    if ($LASTEXITCODE -ne 0) { throw 'failed to extract archive' }

    $bin = Get-ChildItem -Path $tmp -Recurse -Filter 'lci.exe' | Select-Object -First 1
    if (-not $bin) { throw 'extracted archive did not contain lci.exe' }

    Copy-Item -Path $bin.FullName -Destination (Join-Path $prefix 'lci.exe') -Force
    Write-Output "Installed lci to $prefix\lci.exe"

    # Add to the user PATH (persistent, future shells) if missing, and to the
    # current session so lci is immediately callable — `irm | iex` runs in the
    # caller's session, so updating $env:PATH here takes effect now.
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (($userPath -split ';') -notcontains $prefix) {
        [Environment]::SetEnvironmentVariable('Path', "$userPath;$prefix", 'User')
        Write-Output "Added $prefix to your user PATH."
    }
    if (($env:PATH -split ';') -notcontains $prefix) {
        $env:PATH = "$prefix;$env:PATH"
    }
    & (Join-Path $prefix 'lci.exe') --version

    # Register the lci MCP server (`lci mcp`, stdio) with slop-mcp when it is
    # installed, so the binary is immediately usable as an MCP. User scope = all
    # projects. Re-running overwrites the entry. Skip with $env:LCI_NO_SLOP='1'.
    if ($env:LCI_NO_SLOP -ne '1' -and (Get-Command slop-mcp -ErrorAction SilentlyContinue)) {
        & slop-mcp mcp add --user lci (Join-Path $prefix 'lci.exe') mcp 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output 'Registered lci MCP server with slop-mcp (user scope).'
        } else {
            Write-Warning "slop-mcp found but registering lci failed; add it manually: slop-mcp mcp add --user lci `"$prefix\lci.exe`" mcp"
        }
    }

    Write-Output 'Update later with:'
    Write-Output '  lci update            # install latest release'
    Write-Output '  lci update --check    # report current vs latest without installing'

    # Optional weekly auto-update via Task Scheduler. Opt in with
    # $env:LCI_AUTO_UPDATE='1'. /F overwrites an existing task (idempotent).
    if ($env:LCI_AUTO_UPDATE -eq '1') {
        $exe = Join-Path $prefix 'lci.exe'
        & schtasks /Create /TN 'lci-update' /TR "`"$exe`" update" /SC WEEKLY /F 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Output "Auto-update enabled: weekly scheduled task 'lci-update'."
            Write-Output '  Disable: schtasks /Delete /TN lci-update /F'
        } else {
            Write-Warning 'LCI_AUTO_UPDATE=1 but creating the scheduled task failed.'
        }
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
