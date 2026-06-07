# lci installer for Windows — downloads the matching prebuilt binary from
# GitHub releases.
#
#   irm https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.ps1 | iex
#
# Environment:
#   LCI_VERSION   pin a specific release (e.g. 0.6.0); default: latest
#   LCI_PREFIX    install directory; default: %LOCALAPPDATA%\Programs\lci
#   LCI_DRYRUN    set to 1 to print the resolved download URL and exit
#
# Contract: see docs/superpowers/specs/2026-06-07-install-update-distribution-design.md
$ErrorActionPreference = 'Stop'

$repo = 'standardbeagle/lci-cpp'

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
    $tarball = Join-Path $tmp 'lci.tar.gz'
    Write-Output "Downloading $($asset.browser_download_url)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tarball -Headers @{ 'User-Agent' = 'lci-installer' }

    tar -xf $tarball -C $tmp
    if ($LASTEXITCODE -ne 0) { throw 'failed to extract archive' }

    $bin = Get-ChildItem -Path $tmp -Recurse -Filter 'lci.exe' | Select-Object -First 1
    if (-not $bin) { throw 'extracted archive did not contain lci.exe' }

    Copy-Item -Path $bin.FullName -Destination (Join-Path $prefix 'lci.exe') -Force
    Write-Output "Installed lci to $prefix\lci.exe"

    # Add to the user PATH if missing.
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (($userPath -split ';') -notcontains $prefix) {
        [Environment]::SetEnvironmentVariable('Path', "$userPath;$prefix", 'User')
        Write-Output "Added $prefix to your user PATH. Restart your shell to pick it up."
    }
    & (Join-Path $prefix 'lci.exe') --version
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
