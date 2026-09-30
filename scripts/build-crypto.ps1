# build-crypto.ps1 -- the OS-nonce AES-GCM library (native/os_nonce_crypto.cpp).
#
# Windows twin of build-crypto.sh. Called by build-duckdb.ps1 on EVERY run, the cache
# hit included: it takes a second, and a `nova_duckdb_crypto` missing from native/lib
# would make the whole package SKIP silently (a library named in [ffi] libs and absent
# on disk degrades the package, it does not fail it).
#
# WHY A LIBRARY AND NOT A SHIM: `[ffi] c_shims` compiles C only -- a `.cpp` listed
# there is skipped without a word (measured 2026-09-30: `undefined symbol:
# ddb_install_os_nonce_crypto`). The file uses DuckDB's C++ classes, so it is built by
# the SAME compiler with the SAME flags as DuckDB itself (cl.exe through vcvars64,
# /MT /EHsc): a C++ ABI mismatch with duckdb_static would not fail to link, it would
# fail at run time.

param(
  # Empty means "find it", the way build-duckdb.ps1 does.
  [string]$VcVars = ""
)

function FindVcVars {
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
$src = Join-Path $root "native\os_nonce_crypto.cpp"
$work = Join-Path $root "native\build\crypto"
$lib = Join-Path $root "native\lib"
$obj = Join-Path $work "os_nonce_crypto.obj"
$out = Join-Path $lib "nova_duckdb_crypto.lib"
$inc1 = Join-Path $root "native\duckdb\src\include"
$inc2 = Join-Path $root "native\duckdb\third_party\mbedtls\include"
# The same symbol prefix DuckDB's mbedTLS is built with -- see build-duckdb.ps1.
$prefix = Join-Path $root "native\duckdb_mbedtls_prefix.h"

if (-not $VcVars) { $VcVars = FindVcVars }
if (-not $VcVars) { throw "vcvars64.bat not found on D: or C: for any 2022 edition. Pass -VcVars <path>." }
New-Item -ItemType Directory -Force -Path $work, $lib | Out-Null

"crypto: compiling $src"
cmd /c "call `"$VcVars`" >nul 2>&1 && cl /nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /DNDEBUG /DDUCKDB_STATIC_BUILD /FI`"$prefix`" /I`"$inc1`" /I`"$inc2`" `"$src`" /Fo`"$obj`" && lib /nologo /OUT:`"$out`" `"$obj`""
if ($LASTEXITCODE -ne 0) { throw "building nova_duckdb_crypto failed with $LASTEXITCODE" }
"crypto: $out"
