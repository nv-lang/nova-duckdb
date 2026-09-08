# build-duckdb.ps1 — build DuckDB from the pinned submodule into native/lib.
#
# Source: subplan 01.4 §2 of claude-limits.
#
# Run once. The result is a set of static libraries plus `scripts/libs-manifest.txt`
# naming them in link order; `nova.toml`'s `[ffi] libs` is filled from that file.
#
#   pwsh -File scripts/build-duckdb.ps1
#   pwsh -File scripts/build-duckdb.ps1 -Force        # ignore the cache stamp
#
# ── Why the amalgamation is not enough ──────────────────────────────────────
#
# The spike of 2026-09-06 built `libduckdb-src.zip` with one `clang-cl` in eleven
# minutes and it worked — for everything except `date_trunc`, `time_bucket` and
# TIMESTAMPTZ arithmetic, which live in the `core_functions` and `icu` extensions
# that the amalgamation does not contain. With extension autoloading on, the first
# call to one of those goes to the internet and hangs for twenty-two seconds before
# failing. The schema of 01.2 is built on exactly those functions, so this script
# builds the repository with both extensions linked in, and the shim turns
# autoloading off so the failure mode cannot come back.
#
# ── Requirements ────────────────────────────────────────────────────────────
#
#   CMake >= 3.20, Ninja, LLVM (clang-cl), Visual Studio Build Tools for vcvars64.
#   8 GB RAM. Expect 40-60 minutes on 16 cores with a cold cache.
#
# Measured 2026-09-08 on the machine this package was written on: CMake and Ninja
# were ABSENT, and `C:\Program Files\Microsoft Visual Studio\2022` was empty even
# though the spike had used its vcvars64 two days earlier. The script therefore
# checks for its tools and says which one is missing rather than failing halfway
# through a CMake configure.

[CmdletBinding()]
param(
  [string]$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
  [string]$Llvm   = "C:\Program Files\LLVM\bin",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$submodule = Join-Path $root "native\duckdb"
$build = Join-Path $root "native\build"
$lib = Join-Path $root "native\lib"
$stamp = Join-Path $lib ".built"
$manifest = Join-Path $PSScriptRoot "libs-manifest.txt"

# The libraries CMake produces, dependent before dependency. Regenerated below from
# what actually lands in the build tree -- this list is the EXPECTATION, and a
# mismatch is reported rather than silently accepted, because a vendored dependency
# appearing or vanishing between DuckDB versions is exactly the risk 01.4 §8 names.
$expected = @(
  "core_functions_extension", "icu_extension", "duckdb_static",
  "duckdb_re2", "duckdb_fmt", "duckdb_utf8proc", "duckdb_hyperloglog",
  "duckdb_fastpforlib", "duckdb_skiplistlib", "duckdb_mbedtls", "duckdb_yyjson",
  "duckdb_fsst", "duckdb_zstd", "duckdb_miniz"
)

function Need($name, $path) {
  if (-not (Test-Path $path)) { throw "$name not found at '$path'. See the Requirements block at the top of this script." }
}

function NeedCmd($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "$name is not on PATH. This script needs CMake >= 3.20 and Ninja; neither ships with LLVM or with Git for Windows."
  }
}

# ── the cache stamp ──────────────────────────────────────────────────────────
#
# Keyed on what can change the output: the submodule commit, the flags, and the
# compiler version. Anything else -- the clock, the working directory, who ran it --
# is deliberately NOT in the key, because a cache that misses for those reasons is
# a cache that costs forty minutes for nothing.

function CacheKey {
  $commit = (& git -C $submodule rev-parse HEAD 2>$null)
  if (-not $commit) { $commit = "no-submodule" }
  $clang = (& "$Llvm\clang-cl.exe" --version 2>$null | Select-Object -First 1)
  $flags = ($expected -join ",") + "|core_functions;icu|MultiThreaded|Release"
  $text = "$commit|$clang|$flags"
  $sha = [System.Security.Cryptography.SHA256]::Create()
  ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) | ForEach-Object { $_.ToString("x2") }) -join ""
}

$key = CacheKey
if ((-not $Force) -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $key)) {
  "cache hit ($key) - nothing to build. Pass -Force to rebuild."
  exit 0
}

# ── checks before the forty minutes ─────────────────────────────────────────

Need "the DuckDB submodule" (Join-Path $submodule "CMakeLists.txt")
Need "vcvars64.bat" $VcVars
Need "clang-cl" (Join-Path $Llvm "clang-cl.exe")
NeedCmd "cmake"
NeedCmd "ninja"

New-Item -ItemType Directory -Force -Path $lib | Out-Null

# ── configure and build ─────────────────────────────────────────────────────
#
# `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` is /MT, matching Nova's runtime; the
# spike proved there is no CRT conflict that way and the resulting binary imports no
# CRT DLL at all. Extension autoloading and autoinstall are off at BUILD time as
# well as at connection time -- two locks on the same door, because the failure they
# prevent is a twenty-two second hang against the network.

$cmakeArgs = @(
  "-S", $submodule, "-B", $build, "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DBUILD_EXTENSIONS=core_functions;icu",
  "-DENABLE_EXTENSION_AUTOLOADING=0",
  "-DENABLE_EXTENSION_AUTOINSTALL=0",
  "-DBUILD_SHELL=0", "-DBUILD_UNITTESTS=0", "-DBUILD_BENCHMARKS=0",
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
) -join " "

$sw = [Diagnostics.Stopwatch]::StartNew()
"configure..."
cmd /c "call `"$VcVars`" >nul 2>&1 && cmake $cmakeArgs"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with $LASTEXITCODE" }

"build (expect 40-60 min on 16 cores with a cold cache)..."
cmd /c "call `"$VcVars`" >nul 2>&1 && cmake --build `"$build`" --target duckdb_static -j"
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with $LASTEXITCODE" }
"built in $([int]$sw.Elapsed.TotalMinutes) min"

# ── collect and check ───────────────────────────────────────────────────────

$found = @()
Get-ChildItem -Path $build -Recurse -Filter "*.lib" | ForEach-Object {
  Copy-Item $_.FullName -Destination $lib -Force
  $found += $_.BaseName
}

$missing = $expected | Where-Object { $found -notcontains $_ }
$extra = $found | Where-Object { $expected -notcontains $_ }
if ($missing) { "MISSING (expected but not produced): $($missing -join ', ')" }
if ($extra) { "EXTRA (produced but not expected): $($extra -join ', ')" }
if ($missing -or $extra) {
  "The library set changed. Update nova.toml's [ffi] libs from the manifest below, in dependent-before-dependency order, and say so in the commit."
}

# The manifest is what `[ffi] libs` is filled from, so it is written from what was
# actually produced -- ordered by the expectation first, then whatever is new.
$ordered = @()
$ordered += $expected | Where-Object { $found -contains $_ }
$ordered += $extra
Set-Content -Path $manifest -Value $ordered -Encoding utf8

Set-Content -Path $stamp -Value $key -Encoding utf8
"manifest: $manifest ($($ordered.Count) libraries)"
"stamp:    $stamp"
