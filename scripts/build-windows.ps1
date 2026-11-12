<#
.SYNOPSIS
  Native Windows build of DeckHand with MSVC and vcpkg. NOT TESTED where it was written (no Windows machine was
  available); the MinGW cross-build in scripts/windows/docker-build.sh is the verified path. Report problems.

.DESCRIPTION
  Needs, installed beforehand (the script installs nothing system-wide):
    * Visual Studio 2022 (or Build Tools) with "Desktop development with C++" (MSVC v143, Windows 10/11 SDK, CMake)
    * git
    * vcpkg, cloned and bootstrapped somewhere (set VCPKG_ROOT or pass -VcpkgRoot); the script runs
      `vcpkg install` for the libraries below into vcpkg's own folder.
  Libraries (static, x64): openssl, zlib, sqlcipher  (triplet x64-windows-static: static CRT, no DLLs to ship).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Test
  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -BuildDir E:\deckhand\build -Test -Screenshot
#>
param(
  [string]$VcpkgRoot = $env:VCPKG_ROOT,
  [string]$BuildDir = "build-windows",
  [ValidateSet("Release", "RelWithDebInfo", "Debug")] [string]$Config = "Release",
  [switch]$Test,         # run ctest after building
  [switch]$Screenshot,   # run deckhand.exe --screenshot (needs a GPU or WARP) and write shell.png into the build folder
  [switch]$SkipDeps      # do not run `vcpkg install` (already done)
)
$ErrorActionPreference = "Stop"
$Triplet = "x64-windows-static"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not $VcpkgRoot -or -not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
  throw "vcpkg not found. Clone https://github.com/microsoft/vcpkg, run bootstrap-vcpkg.bat, and set VCPKG_ROOT (or pass -VcpkgRoot)."
}
$Vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
$Cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
if (-not $Cmake) {
  # CMake that ships with Visual Studio
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    $candidate = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $candidate) { $Cmake = Get-Item $candidate }
  }
}
if (-not $Cmake) { throw "cmake not found (install the 'C++ CMake tools for Windows' component of Visual Studio)." }
$CmakeExe = if ($Cmake.PSObject.Properties.Name -contains "Source") { $Cmake.Source } else { $Cmake.FullName }

if (-not $SkipDeps) {
  & $Vcpkg install "openssl:$Triplet" "zlib:$Triplet" "sqlcipher:$Triplet"
  if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }
}
$Prefix = Join-Path $VcpkgRoot "installed\$Triplet"

# SQLCipher's vcpkg port installs sqlite3.h under include\sqlcipher; CMakeLists.txt looks there (SQLCIPHER_ROOT / CMAKE_PREFIX_PATH).
$Build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
& $CmakeExe -S $Root -B $Build -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" "-DVCPKG_TARGET_TRIPLET=$Triplet" `
  "-DCMAKE_PREFIX_PATH=$Prefix" "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" "-DOPENSSL_USE_STATIC_LIBS=ON"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $CmakeExe --build $Build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$Bin = Join-Path $Build $Config
Write-Host "`nBuilt:"
Get-ChildItem $Bin -Filter *.exe | Where-Object { $_.Name -notlike "test_*" } | ForEach-Object {
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
  Write-Host ("{0}  {1}" -f $h, $_.Name)
}

if ($Test) {
  Push-Location $Build
  try { & (Join-Path (Split-Path $CmakeExe) "ctest.exe") -C $Config --output-on-failure --timeout 600 } finally { Pop-Location }
}

if ($Screenshot) {
  # Resources (controllers, lang, skins) sit beside the exe in share\deckhand: the POST_BUILD step copies them to <build>\share.
  $env:DECKHAND_RESOURCE_DIR = Join-Path $Build "share\deckhand"
  $png = Join-Path $Build "shell.png"
  & (Join-Path $Bin "deckhand.exe") --screenshot $png --frames 60
  if (Test-Path $png) { Write-Host "screenshot: $png" } else { Write-Warning "no screenshot was written" }
}
