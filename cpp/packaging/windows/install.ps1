# Crowsnest PowerShell installer (Windows 10+).
#
# Bootstrap one-liner (no Git Bash needed):
#   irm https://raw.githubusercontent.com/codeafridi/Crowsnest/main/cpp/packaging/windows/install.ps1 | iex
#
# Options (irm|iex cannot take parameters - they come from env):
#   $env:CROWSNEST_UNINSTALL='1';    irm ... | iex
#   $env:CROWSNEST_AUTOSTART='1';    irm ... | iex   start the HUD now and at login
#   $env:CROWSNEST_NO_AUTOSTART='1'; irm ... | iex   (accepted, no effect - default is off)
#   $env:CROWSNEST_WIN_URL='<zip>';  irm ... | iex
# Direct invocation takes the same options as parameters:
#   ./install.ps1 [-Uninstall] [-Autostart] [-NoAutostart] [-Url <zip url>]
#
# Installs the app and the `crowsnest` command. Nothing starts automatically:
# run `crowsnest` after installing, or pass -Autostart to start the HUD now
# and at every login. `crowsnest update` runs this script again (it stops the
# running HUD first, because Windows locks a running exe). Downloads show live
# byte/percent progress.

#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$Autostart,
    [switch]$NoAutostart,
    [string]$Url
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Repo = 'codeafridi/Crowsnest'

if ($PSVersionTable.PSVersion.Major -lt 6) {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

function Info([string]$msg) { Write-Host $msg }
function Die([string]$msg) { throw "error: $msg" }

if ($env:CROWSNEST_UNINSTALL) { $Uninstall = $true }
if ($env:CROWSNEST_AUTOSTART) { $Autostart = $true }
if ($env:CROWSNEST_NO_AUTOSTART) { $NoAutostart = $true }

function Format-Bytes([long]$n) {
    if ($n -ge 1GB) { return '{0:F1} GB' -f ($n / 1GB) }
    if ($n -ge 1MB) { return '{0:F1} MB' -f ($n / 1MB) }
    if ($n -ge 1KB) { return '{0:F1} KB' -f ($n / 1KB) }
    return "$n B"
}

# Streams the body straight to disk with a live progress line that rewrites
# itself in place. A read that stops producing data for 30 s aborts, a body
# shorter than its Content-Length aborts, and the whole download is retried
# once before giving up. Redirected output (CI) prints nothing per chunk.
function Invoke-DownloadOnce {
    param([string]$Source, [string]$Dest)
    $null = Add-Type -AssemblyName System.Net.Http -ErrorAction SilentlyContinue
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromSeconds(600)
    try {
        $resp = $client.GetAsync($Source,
            [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead
        ).GetAwaiter().GetResult()
        try {
            if (-not $resp.IsSuccessStatusCode) {
                throw "HTTP $([int]$resp.StatusCode) for $Source"
            }
            $total = $resp.Content.Headers.ContentLength
            $stream = $resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $out = [IO.File]::Create($Dest)
            try {
                $buf = New-Object byte[] 65536
                $done = [long]0
                $sw = [Diagnostics.Stopwatch]::StartNew()
                $lastData = [long]0
                $lastTick = [long]0
                $live = -not [Console]::IsOutputRedirected
                while ($true) {
                    $read = $stream.ReadAsync($buf, 0, $buf.Length)
                    while (-not $read.Wait(1000)) {
                        if ($sw.ElapsedMilliseconds - $lastData -gt 30000) {
                            throw 'download stalled: no data for 30 s'
                        }
                    }
                    $n = $read.Result
                    if ($n -eq 0) { break }
                    $out.Write($buf, 0, $n)
                    $done += $n
                    $lastData = $sw.ElapsedMilliseconds
                    if ($live -and ($sw.ElapsedMilliseconds - $lastTick) -ge 200) {
                        $lastTick = $sw.ElapsedMilliseconds
                        $line = if ($total) {
                            '{0,3}%  ({1} of {2})' -f [int](100 * $done / $total),
                                (Format-Bytes $done), (Format-Bytes $total)
                        } else {
                            Format-Bytes $done
                        }
                        [Console]::Write("downloading  $line`r")
                    }
                }
                if ($total -and $done -lt $total) {
                    throw "download truncated: got $(Format-Bytes $done) of $(Format-Bytes $total)"
                }
                if ($live) { [Console]::Write((' ' * 60) + "`r") }
            } finally {
                $out.Dispose()
            }
        } finally {
            $resp.Dispose()
        }
    } finally {
        $client.Dispose()
    }
}

function DownloadWithProgress {
    param([string]$Source, [string]$Dest)
    foreach ($attempt in 1..2) {
        try {
            Invoke-DownloadOnce -Source $Source -Dest $Dest
            $size = [long](Get-Item -LiteralPath $Dest).Length
            Info "downloaded: $(Format-Bytes $size) -> $Dest"
            return
        } catch {
            Remove-Item -LiteralPath $Dest -Force -ErrorAction SilentlyContinue
            if ($attempt -eq 2) { throw "download failed: $($_.Exception.Message)" }
            Info "download attempt $attempt failed, retrying"
        }
    }
}

function LatestTag {
    if ($env:CROWSNEST_TEST_TAG) { return $env:CROWSNEST_TEST_TAG }
    $null = Add-Type -AssemblyName System.Net.Http -ErrorAction SilentlyContinue
    $h = New-Object System.Net.Http.HttpClientHandler
    $h.AllowAutoRedirect = $false
    $c = New-Object System.Net.Http.HttpClient($h)
    try {
        $resp = $c.GetAsync("https://github.com/$Repo/releases/latest").GetAwaiter().GetResult()
        $loc = $resp.Headers.Location
        if (-not $loc) { throw 'error: could not resolve the latest release' }
        $tag = ($loc.ToString() -split '/')[-1]
        if (-not $tag) { throw 'error: could not resolve the latest release' }
        Info "latest release: $tag"
        return $tag
    } finally {
        $c.Dispose()
    }
}

function ZipUrl {
    if ($env:CROWSNEST_WIN_URL) { return $env:CROWSNEST_WIN_URL }
    if ($Url) { return $Url }
    $ver = ($tag -replace '^v', '')
    return "https://github.com/$Repo/releases/download/$tag/Crowsnest-$ver-windows-portable.zip"
}

# Resolved once: the tag names the release AND the VERSION stamp written into
# the install directory, which is what `update` reports as old -> new.
$tag = $null
if (-not $env:CROWSNEST_WIN_URL -and -not $Url) {
    $tag = LatestTag
}

$AppDir = Join-Path $env:LOCALAPPDATA 'Crowsnest'
$ShimDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps'
$Exe = Join-Path $AppDir 'crowsnest.exe'
$VerFile = Join-Path $AppDir 'VERSION'

# A running exe keeps its image locked: it is stopped first (the exe's own
# `stop` verb waits for the kernel to retire each process), and folder removal
# is retried briefly because the handle is released a moment later.
function Stop-RunningHud {
    $stopped = 0
    if (Test-Path $Exe) {
        # PowerShell does not wait for a GUI-subsystem exe invoked with `&`,
        # so the count would never be read; cmd.exe waits and passes the
        # output through. Elsewhere (tests on other OSes) call it directly.
        $out = ''
        try {
            if ($env:OS -eq 'Windows_NT') {
                $out = cmd /c "`"$Exe`" stop" 2>$null
            } else {
                $out = & $Exe stop 2>$null
            }
        } catch { $out = '' }
        if ("$out" -match 'stopped:\s*(\d+)') { $stopped = [int]$Matches[1] }
    }
    $procs = @(Get-Process -Name crowsnest -ErrorAction SilentlyContinue)
    if ($procs.Count -gt 0) {
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        $stopped += $procs.Count
    }
    return ($stopped -gt 0)
}

function Remove-AppDir {
    for ($i = 0; $i -lt 50; $i++) {
        if (-not (Test-Path $AppDir)) { return }
        try { Remove-Item -Recurse -Force $AppDir; return } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (Test-Path $AppDir) {
        throw "could not replace $AppDir - close any file open in it and retry"
    }
}

if ($Uninstall) {
    if (Test-Path $Exe) {
        try { & $Exe --autostart=off 2>$null | Out-Null } catch { }
    }
    $null = Stop-RunningHud
    Remove-AppDir
    $shim = Join-Path $ShimDir 'crowsnest.cmd'
    $shimPosix = Join-Path $ShimDir 'crowsnest'
    if (Test-Path $shim) { Remove-Item -Force $shim }
    if (Test-Path $shimPosix) { Remove-Item -Force $shimPosix }
    Info 'Crowsnest uninstalled (Windows).'
    return
}

Info 'Downloading Crowsnest for Windows...'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('crowsnest-win-' + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp 'crowsnest.zip'
    $zipUrl = ZipUrl
    if ($zipUrl -match '^(?i)file:/') {
        $localZip = [Uri]::UnescapeDataString(([Uri]$zipUrl).LocalPath)
        Copy-Item -LiteralPath $localZip -Destination $zip -Force
        Info "using local bundle: $localZip"
    } elseif ($zipUrl -match '^[a-zA-Z]:[\\/]') {
        Copy-Item -LiteralPath $zipUrl -Destination $zip -Force
        Info "using local bundle: $zipUrl"
    } else {
        DownloadWithProgress -Source $zipUrl -Dest $zip
    }
    if ((Get-Item -LiteralPath $zip).Length -lt 10MB) { throw 'downloaded zip is too small' }

    $ver = 'unknown'
    if ($tag) { $ver = $tag -replace '^v', '' }
    if ($env:CROWSNEST_TEST_TAG) { $ver = $env:CROWSNEST_TEST_TAG -replace '^v', '' }
    $oldVer = $null
    if (Test-Path $VerFile) { $oldVer = (Get-Content -LiteralPath $VerFile -Raw).Trim() }

    if (Stop-RunningHud) { Info 'stopped the running HUD to replace its files' }
    Remove-AppDir
    New-Item -ItemType Directory -Path $AppDir | Out-Null
    Info "extracting to $AppDir"
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $AppDir)
    } catch {
        Expand-Archive -Path $zip -DestinationPath $AppDir -Force
    }
    if (-not (Test-Path $Exe)) {
        Die 'portable install incomplete (crowsnest.exe missing)'
    }
    [IO.File]::WriteAllText($VerFile, $ver + "`n", [Text.Encoding]::ASCII)

    New-Item -ItemType Directory -Path $ShimDir -Force | Out-Null
    $shimCmd = Join-Path $ShimDir 'crowsnest.cmd'
    # A bare `crowsnest` must return the prompt immediately. Calling a GUI
    # executable directly from cmd.exe waits for the HUD to exit; verbs with
    # arguments stay synchronous so their output/exit status remains useful.
    $shimCmdContents = (@(
        '@echo off',
        'if "%~1"=="" (',
        '  start "" /b "%LOCALAPPDATA%\Crowsnest\crowsnest.exe"',
        '  exit /b 0',
        ')',
        '"%LOCALAPPDATA%\Crowsnest\crowsnest.exe" %*'
    ) -join "`r`n") + "`r`n"
    [IO.File]::WriteAllBytes($shimCmd,
        [Text.Encoding]::ASCII.GetBytes($shimCmdContents))
    $shimPosix = Join-Path $ShimDir 'crowsnest'
    $shimPosixContents = (@(
        '#!/bin/sh',
        'if [ "$#" -eq 0 ]; then',
        '  cmd.exe //c start "" /b "$LOCALAPPDATA\Crowsnest\crowsnest.exe" >/dev/null 2>&1',
        '  exit 0',
        'fi',
        'exec "$LOCALAPPDATA/Crowsnest/crowsnest.exe" "$@"'
    ) -join "`n") + "`n"
    [IO.File]::WriteAllBytes($shimPosix,
        [Text.Encoding]::ASCII.GetBytes($shimPosixContents))

    if ($Autostart) {
        $out = Join-Path $tmp 'autostart.out'
        $err = Join-Path $tmp 'autostart.err'
        $p = Start-Process -FilePath $Exe -ArgumentList '--autostart=on' `
                -Wait -PassThru `
                -RedirectStandardOutput $out -RedirectStandardError $err
        if ($p.ExitCode -ne 0) {
            $detail = ((Get-Content $err -Raw -ErrorAction SilentlyContinue) + `
                       (Get-Content $out -Raw -ErrorAction SilentlyContinue)).Trim()
            throw "autostart failed: exit $($p.ExitCode) - $detail"
        }
        Start-Process -FilePath $Exe
        Info 'Started now and at login.'
    } else {
        Info 'Nothing is running yet - open a NEW shell and type: crowsnest'
    }

    Info ''
    if ($oldVer -and $oldVer -ne $ver) {
        Info "Updated $oldVer -> $ver in $AppDir"
    } elseif ($oldVer) {
        Info "Reinstalled $ver in $AppDir"
    } else {
        Info "Installed $ver to $AppDir"
    }
    Info ''
    Info 'Next steps:'
    Info '  crowsnest                 launch the HUD and return to the prompt'
    Info '  crowsnest stop            stop every running HUD instance'
    Info '  crowsnest status          version, autostart state, running instances'
    Info '  crowsnest restart         restart the HUD'
    Info '  crowsnest enable          start now and at every login'
    Info '  crowsnest disable         stop and do not start at login'
    Info '  crowsnest update          download and install the latest release'
    Info '  crowsnest uninstall       remove the app, the command and autostart'
    Info '  crowsnest path            show where it is installed'
    Info '  crowsnest vers            show the installed version'
    Info '  crowsnest --dump          JSON snapshot of the machine (headless check)'
    Info "note: unsigned build - SmartScreen 'More info / Run anyway' on first GUI launch."
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
