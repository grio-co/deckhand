<#
.SYNOPSIS
  Real-application check of the separate engine process on Windows (the counterpart of scripts/linux/engine-check.sh).

.DESCRIPTION
  Runs the built deckhand.exe (the real app, with its window) against a throw-away portable data folder, drives it
  through its REST API (http://127.0.0.1:7373/v1, bearer token from <data>\api-admin-token.txt) and checks:
    1. the app starts deckhand-engine.exe, a track plays (master muted first), the helper has no window;
    2. the UI is killed (Stop-Process -Force = TerminateProcess): the helper keeps playing, same pid;
    3. a relaunched UI re-attaches (same helper) and the position continued by the wall-clock gap;
    4. a killed helper is restarted by the UI;
    5. an orderly quit (WM_CLOSE) stops the helper and leaves no segment, socket file or lock behind.
  There is no sound device requirement: with no output device the engine runs on miniaudio's null device, and the
  master level is set to 0 anyway.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\windows\engine-check.ps1 -BuildDir build-windows -Config Release
#>
param(
  [string]$BuildDir = "build-windows",
  [string]$Config = "Release",
  [string]$ExeDir = ""   # folder with deckhand.exe and deckhand-engine.exe (default: <BuildDir>\<Config>)
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
if (-not $ExeDir) { $ExeDir = Join-Path $Build $Config }
$ShareDir = Join-Path $Build "share"
foreach ($f in "deckhand.exe", "deckhand-engine.exe") {
  if (-not (Test-Path (Join-Path $ExeDir $f))) { throw "missing $f in $ExeDir (build first)" }
}

# ---- a private copy of the app in portable mode: its data folder is DeckHandData\ beside the exe --------------------
$Work = Join-Path ([IO.Path]::GetTempPath()) ("dh-engine-check-" + [Diagnostics.Process]::GetCurrentProcess().Id)
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$App = Join-Path $Work "app"
New-Item -ItemType Directory -Force $App | Out-Null
Copy-Item (Join-Path $ExeDir "deckhand.exe"), (Join-Path $ExeDir "deckhand-engine.exe") $App
if (Test-Path $ShareDir) { Copy-Item -Recurse $ShareDir (Join-Path $App "share") }
New-Item -ItemType File (Join-Path $App "portable") | Out-Null
$Data = Join-Path $App "DeckHandData"
$AppExe = Join-Path $App "deckhand.exe"
$Helper = Join-Path $App "deckhand-engine.exe"

# ---- a 4-minute stereo test tone (16-bit PCM WAV, which the built-in reader decodes) --------------------------------
Add-Type -TypeDefinition @"
using System; using System.IO;
public static class DhTone {
  public static void Write(string path, int seconds) {
    const int rate = 44100; int frames = rate * seconds;
    using (var w = new BinaryWriter(File.Create(path))) {
      w.Write(new char[] {'R','I','F','F'}); w.Write(36 + frames * 4); w.Write(new char[] {'W','A','V','E','f','m','t',' '});
      w.Write(16); w.Write((short)1); w.Write((short)2); w.Write(rate); w.Write(rate * 4); w.Write((short)4); w.Write((short)16);
      w.Write(new char[] {'d','a','t','a'}); w.Write(frames * 4);
      for (int i = 0; i < frames; i++) {
        double t = (double)i / rate; double beat = (t * 2) - Math.Floor(t * 2);
        short v = (short)(32767 * 0.5 * (Math.Exp(-8 * beat) * Math.Sin(6.28318 * 80 * t) + 0.2 * Math.Sin(6.28318 * 440 * t)));
        w.Write(v); w.Write(v);
      }
    }
  }
}
"@
$Tone = Join-Path $Work "tone.wav"
[DhTone]::Write($Tone, 240)

$script:Fail = 0
function Ok($m) { Write-Host "PASS $m" }
function Bad($m) { Write-Host "FAIL $m"; $script:Fail = 1 }

$env:DECKHAND_LOG = "info"
$env:DECKHAND_ENGINE_PROCESS = "separate"
$Base = "http://127.0.0.1:7373/v1"
$script:Token = ""
function Api([string]$Method, [string]$Path, [string]$Body = "") {
  try {
    if (-not $script:Token) {
      $tf = Join-Path $Data "api-admin-token.txt"
      if (Test-Path $tf) { $script:Token = (Get-Content $tf -Raw).Trim() }
    }
    $h = @{ Authorization = "Bearer $($script:Token)" }
    if ($Method -eq "POST") {
      return Invoke-RestMethod -Method Post -Uri "$Base$Path" -Headers $h -TimeoutSec 4 -ContentType "application/json" -Body $(if ($Body) { $Body } else { "{}" })
    }
    return Invoke-RestMethod -Method Get -Uri "$Base$Path" -Headers $h -TimeoutSec 4
  } catch { return $null }
}
function Wait-Api([int]$Seconds = 60) {
  $end = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $end) { if ($null -ne (Api "GET" "/status")) { return $true }; Start-Sleep -Milliseconds 300 }
  return $false
}
function Wait-Until([scriptblock]$Cond, [int]$Seconds) {
  $end = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $end) { if (& $Cond) { return $true }; Start-Sleep -Milliseconds 200 }
  return [bool](& $Cond)
}
function Pos { $d = Api "GET" "/decks/1"; if ($null -eq $d) { return $null }; return [double]$d.position_seconds }
function Engine-Pids { @(Get-Process -Name "deckhand-engine" -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) }
function Start-Ui([string[]]$UiArgs) {
  if ($UiArgs -and $UiArgs.Count -gt 0) { return Start-Process -FilePath $AppExe -ArgumentList $UiArgs -WorkingDirectory $App -PassThru }
  return Start-Process -FilePath $AppExe -WorkingDirectory $App -PassThru
}
function Probe { ((& $Helper --probe --data-dir $Data) | Out-String).Trim() }

$ui = $null
try {
  Write-Host "== 1. start the UI with a track, mute, play"
  $ui = Start-Ui @("--deck-a", $Tone)
  if (Wait-Api) { Ok "API up" } else { Bad "API did not come up"; throw "no API" }
  $null = Api "POST" "/mixer/master_level" '{"value":0}'
  if (Wait-Until { $d = Api "GET" "/decks/1"; $null -ne $d -and $d.loaded } 60) { Ok "track loaded on deck 1" } else { Bad "track did not load"; throw "no track" }
  $eps = Engine-Pids
  if ($eps.Count -eq 1) { $EP = $eps[0]; Ok "engine process running (pid $EP)" } else { Bad "expected one deckhand-engine, found $($eps.Count)"; throw "no helper" }
  $eproc = Get-Process -Id $EP
  if ($eproc.MainWindowHandle -eq 0) { Ok "the helper has no window" } else { Bad "the helper has a window" }
  $null = Api "POST" "/mixer/master_level" '{"value":0}'
  $null = Api "POST" "/decks/1/play"
  Start-Sleep -Seconds 2
  $P1 = Pos; Start-Sleep -Seconds 2; $P2 = Pos
  $T0 = Get-Date
  Write-Host "position $P1 -> $P2"
  if ($null -ne $P1 -and $null -ne $P2 -and $P2 -gt $P1 + 0.5) { Ok "deck 1 advancing" } else { Bad "deck 1 not advancing"; throw "not playing" }

  Write-Host "== 2. kill the UI (TerminateProcess)"
  Stop-Process -Id $ui.Id -Force
  $null = $ui.WaitForExit(10000)
  Start-Sleep -Seconds 4
  $eps = Engine-Pids
  if ($eps.Count -eq 1 -and $eps[0] -eq $EP) { Ok "the helper survived (pid $EP)" } else { Bad "helper gone or changed: $($eps -join ',')" }

  Write-Host "== 3. relaunch the UI: re-attach, position continued"
  $ui = Start-Ui @()
  if (Wait-Api) { Ok "API up again" } else { Bad "API did not come back"; throw "no API" }
  $null = Wait-Until { $d = Api "GET" "/decks/1"; $null -ne $d -and $d.loaded } 20
  $P3 = Pos; $T1 = Get-Date
  $gap = ($T1 - $T0).TotalSeconds
  Write-Host ("position after relaunch {0:N2} (before the kill {1:N2}, wall clock since then {2:N2} s)" -f $P3, $P2, $gap)
  if ($null -ne $P3 -and [math]::Abs(($P3 - $P2) - $gap) -lt 1.5) { Ok "position continued while the UI was dead (+$([math]::Round($P3 - $P2, 1)) s)" } else { Bad "position did not continue by the wall-clock gap" }
  Start-Sleep -Seconds 2
  $P4 = Pos
  if ($null -ne $P4 -and $P4 -gt $P3 + 0.5) { Ok "still advancing after re-attach" } else { Bad "not advancing after re-attach" }
  $eps = Engine-Pids
  if ($eps.Count -eq 1 -and $eps[0] -eq $EP) { Ok "same helper after re-attach" } else { Bad "helper changed" }

  Write-Host "== 4. the helper is killed: the UI restarts it"
  Stop-Process -Id $EP -Force
  $restarted = Wait-Until { $e = Engine-Pids; $e.Count -eq 1 -and $e[0] -ne $EP } 30
  if ($restarted) { $EP2 = (Engine-Pids)[0]; Ok "helper restarted (pid $EP2)" } else { Bad "helper not restarted" }
  if (-not $ui.HasExited) { Ok "UI still alive" } else { Bad "UI died with the helper" }

  Write-Host "== 5. orderly quit (WM_CLOSE) stops the helper"
  $ui.Refresh()
  $closed = $ui.CloseMainWindow()
  if (-not $closed) { Write-Host "no main window handle; asking with taskkill"; & taskkill.exe /PID $ui.Id | Out-Null }
  if ($ui.WaitForExit(30000)) { Ok "UI exited (code $($ui.ExitCode))" } else { Bad "UI did not exit after WM_CLOSE" }
  if (Wait-Until { (Engine-Pids).Count -eq 0 } 15) { Ok "helper stopped after the quit" } else { Bad "helper still running after the quit" }
  $probe = Probe
  Write-Host "probe: $probe"
  if ($probe -match "engine not running" -and $probe -match "main segment absent" -and $probe -match "0 block segments" -and $probe -match "socket absent") { Ok "no named objects left" } else { Bad "objects left behind" }
} catch {
  Write-Host "ABORTED: $($_.Exception.Message)"
  $script:Fail = 1
} finally {
  if ($script:Fail -ne 0) {
    foreach ($log in "deckhand.log", "deckhand-engine.log", "deckhand-engine.out") {
      $p = Join-Path $Data $log
      if (Test-Path $p) { Write-Host "---- $log (tail)"; Get-Content $p -Tail 40 }
    }
  }
  Get-Process -Name "deckhand", "deckhand-engine" -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$App*" } | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 500
  Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
if ($script:Fail -eq 0) { Write-Host "ENGINE CHECK PASSED"; exit 0 }
Write-Host "ENGINE CHECK FAILED"
exit 1
