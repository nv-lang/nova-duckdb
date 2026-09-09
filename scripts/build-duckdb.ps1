# build-duckdb.ps1 — build DuckDB from the pinned submodule into native/lib.
#
# Source: subplan 01.4 §2 of claude-limits.
#
# Run once. The result is a set of static libraries plus `scripts/libs-manifest.txt`
# naming them in link order; `nova.toml`'s `[ffi] libs` is filled from that file.
#
#   pwsh -File scripts/build-duckdb.ps1                                   # PowerShell 7
#   powershell -ExecutionPolicy Bypass -File scripts\build-duckdb.ps1     # Windows PowerShell 5.1
#   ... -Force                                                            # ignore the cache stamp
#
# MEASURED 2026-09-08, at the first real attempt: `pwsh` is ABSENT on the machine this
# package was written on, and the call died in under a second with
# CommandNotFoundException -- in the first minute of a forty-minute machine slot
# somebody else was waiting for. The 5.1 line is what runs here; the script uses
# nothing newer.
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
# CORRECTION, 2026-09-08 evening. This block used to say CMake and Ninja were ABSENT
# on this machine and that `C:\Program Files\Microsoft Visual Studio\2022` was empty
# even though the spike had used its vcvars64. Both halves were wrong in the same way:
# I looked on drive C. Visual Studio 2022 Community is on **D**, and CMake 3.31.6-msvc6
# and Ninja 1.12.1 are bundled inside it -- neither is on PATH, and vcvars64.bat puts
# both there when it runs. A negative answer sounds exactly as confident as a true one,
# which is why the script now SEARCHES for vcvars64 instead of naming one path, and
# checks for its tools THROUGH vcvars rather than in the shell that will not run them.

[CmdletBinding()]
param(
  # Empty means "find it" -- see FindVcVars below. Pass a path to override.
  [string]$VcVars = "",
  [string]$Llvm   = "C:\Program Files\LLVM\bin",
  [switch]$Force
)

function FindVcVars {
  # Both drives and all four editions, because the one thing this script must not do
  # is announce that a tool is missing when it is installed somewhere else. That
  # exact mistake cost this project a request to the owner to install what was
  # already there.
  foreach ($drive in @("D:", "C:")) {
    foreach ($ed in @("Community", "Professional", "Enterprise", "BuildTools")) {
      $p = "$drive\Program Files\Microsoft Visual Studio\2022\$ed\VC\Auxiliary\Build\vcvars64.bat"
      if (Test-Path $p) { return $p }
    }
  }
  return ""
}

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
# `duckdb_pg_query` added 2026-09-08 after the first real build produced it: a
# vendored dependency that exists in v1.5.5 and was not in this list. Exactly the risk
# 01.4 §8 names, and the reason the produced set is compared with the expected one
# rather than assumed.
$expected = @(
  "core_functions_extension", "icu_extension", "duckdb_static",
  "duckdb_re2", "duckdb_fmt", "duckdb_utf8proc", "duckdb_hyperloglog",
  "duckdb_fastpforlib", "duckdb_skiplistlib", "duckdb_mbedtls", "duckdb_yyjson",
  "duckdb_fsst", "duckdb_zstd", "duckdb_miniz", "duckdb_pg_query",
  "duckdb_generated_extension_loader", "parquet_extension"
)

function Need($name, $path) {
  if (-not (Test-Path $path)) { throw "$name not found at '$path'. See the Requirements block at the top of this script." }
}

function NeedCmdVia($vcvars, $name) {
  # Ask the shell that will actually run it. Visual Studio bundles CMake and Ninja
  # and vcvars64 puts them on PATH; PowerShell's own PATH has neither, so checking
  # here would refuse a build that works.
  $found = cmd /c "call `"$vcvars`" >nul 2>&1 && where $name 2>nul"
  if ($LASTEXITCODE -ne 0 -or -not $found) {
    throw "$name is not available even after vcvars64. This script needs CMake >= 3.20 and Ninja; Visual Studio bundles both under Common7\IDE\CommonExtensions\Microsoft\CMake."
  }
  ($found | Select-Object -First 1)
}

# The cmake flags are defined HERE, above the cache stamp, because the stamp is
# keyed on them: a key computed before its inputs exist is a key of nothing.
$cmakeArgs = @(
  "-S", $submodule, "-B", $build, "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DBUILD_EXTENSIONS=core_functions;icu",
  "-DENABLE_EXTENSION_AUTOLOADING=0",
  "-DENABLE_EXTENSION_AUTOINSTALL=0",
  "-DDISABLE_EXTENSION_LOAD=TRUE",
  "-DBUILD_SHELL=0", "-DBUILD_UNITTESTS=0", "-DBUILD_BENCHMARKS=0",
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
) -join " "

# ── the cache stamp ──────────────────────────────────────────────────────────
#
# Keyed on what can change the output: the submodule commit, the flags, and the
# compiler version. Anything else -- the clock, the working directory, who ran it --
# is deliberately NOT in the key, because a cache that misses for those reasons is
# a cache that costs forty minutes for nothing.
#
# Correction 2026-09-08. This comment was true and the code was not. The key was built
# from $expected (the list of expected LIBRARIES) plus three strings retyped by hand,
# so the actual cmake flags were never in it. Adding -DDISABLE_EXTENSION_LOAD -- a flag
# that changes every object file -- printed "cache hit, nothing to build", and the old
# library would have been accepted as the new one. A cache that answers for inputs it
# does not read is worse than no cache: it is silent, fast, and wrong.
#
# The key now hashes $cmakeArgs itself -- the same string handed to cmake, not a copy
# of it. A copy is exactly what broke this.

function CacheKey {
  $commit = (& git -C $submodule rev-parse HEAD 2>$null)
  if (-not $commit) { $commit = "no-submodule" }
  $clang = (& "$Llvm\clang-cl.exe" --version 2>$null | Select-Object -First 1)
  # $cmakeArgs, not a hand-written echo of it. $expected stays in the key as well: it
  # names the archives the build must produce, and changing that list changes the output.
  $text = "$commit|$clang|$cmakeArgs|" + ($expected -join ",")
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
if (-not $VcVars) { $VcVars = FindVcVars }
if (-not $VcVars) {
  throw "vcvars64.bat not found on D: or C: for any 2022 edition. Pass -VcVars <path>."
}
"vcvars64: $VcVars"
Need "vcvars64.bat" $VcVars
Need "clang-cl" (Join-Path $Llvm "clang-cl.exe")
"cmake:  $(NeedCmdVia $VcVars 'cmake')"
"ninja:  $(NeedCmdVia $VcVars 'ninja')"

New-Item -ItemType Directory -Force -Path $lib | Out-Null

# ── configure and build ─────────────────────────────────────────────────────
#
# `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` is /MT, matching Nova's runtime; the
# spike proved there is no CRT conflict that way and the resulting binary imports no
# CRT DLL at all. Extension autoloading and autoinstall are off at BUILD time as
# well as at connection time -- two locks on the same door, because the failure they
# prevent is a twenty-two second hang against the network.


$sw = [Diagnostics.Stopwatch]::StartNew()
"configure..."
cmd /c "call `"$VcVars`" >nul 2>&1 && cmake $cmakeArgs"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with $LASTEXITCODE" }

"build (expect 40-60 min on 16 cores with a cold cache)..."
# THREE targets, not one. The first real run asked for `duckdb_static` alone and the
# manifest check reported core_functions_extension and icu_extension MISSING: they are
# separate cmake targets and not dependencies of the static library, so nothing built
# them. Their directories were there under native/build/extension/ with CMakeFiles and
# no .lib -- the shape of a target nobody asked for.
#
# It matters because the header of this script exists to explain that the amalgamation
# was abandoned FOR those two extensions: date_trunc, time_bucket and TIMESTAMPTZ
# arithmetic live in them, and the schema of subplan 01.2 is built on those functions.
# Building only duckdb_static reproduced the very gap the repository build was chosen
# to close.
cmd /c "call `"$VcVars`" >nul 2>&1 && cmake --build `"$build`" --target duckdb_static core_functions_extension icu_extension parquet_extension duckdb_generated_extension_loader -j"
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
# WITHOUT a byte-order mark. `-Encoding utf8` on Windows PowerShell 5.1 means WITH
# one, and the mark rode into the first name in the file -- enough to make a literal
# comparison in check-libs-manifest.py report a library as both missing and extra.
[System.IO.File]::WriteAllLines($manifest, $ordered, (New-Object System.Text.UTF8Encoding($false)))

Set-Content -Path $stamp -Value $key -Encoding utf8
"manifest: $manifest ($($ordered.Count) libraries)"
"stamp:    $stamp"
