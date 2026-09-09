#!/usr/bin/env bash
# build-duckdb.sh — build DuckDB from the pinned submodule into native/lib (Linux,
# macOS). The Windows twin is build-duckdb.ps1 and the two must stay in step: same
# flags, same cache key, same manifest.
#
# Source: subplan 01.4 §2 of claude-limits.
#
#   ./scripts/build-duckdb.sh
#   FORCE=1 ./scripts/build-duckdb.sh      # ignore the cache stamp
#
# Requirements: CMake >= 3.20, Ninja, clang++ (or gcc), 8 GB RAM. Expect 40-60
# minutes on 16 cores with a cold cache; the spike measured 20 minutes for the
# amalgamation alone on 10 vCPUs, and this builds the repository with two
# extensions.
#
# Why the repository and not the amalgamation: `core_functions` and `icu` are not in
# `libduckdb-src.zip`, and `date_trunc`, `time_bucket` and TIMESTAMPTZ arithmetic
# live there. Without them DuckDB goes to the network on first use and hangs 22 s
# (spike). The schema of 01.2 is built on those functions.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
submodule="$root/native/duckdb"
build="$root/native/build"
lib="$root/native/lib"
stamp="$lib/.built"
manifest="$here/libs-manifest.txt"

# Dependent before dependency. This is the EXPECTATION; a mismatch after the build
# is reported rather than swallowed (01.4 §8: the vendored set changes between
# DuckDB versions).
expected=(
  core_functions_extension icu_extension duckdb_static
  duckdb_re2 duckdb_fmt duckdb_utf8proc duckdb_hyperloglog
  duckdb_fastpforlib duckdb_skiplistlib duckdb_mbedtls duckdb_yyjson
  duckdb_fsst duckdb_zstd duckdb_miniz
)

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: '$1' is not on PATH. See the Requirements block at the top of this script." >&2
    exit 1
  }
}

[ -f "$submodule/CMakeLists.txt" ] || {
  echo "error: the DuckDB submodule is not checked out at $submodule" >&2
  echo "       git submodule update --init --depth 1 native/duckdb" >&2
  exit 1
}
need_cmd cmake
need_cmd ninja

# ── the cmake flags, defined ONCE ────────────────────────────────────────────
#
# Above the cache stamp on purpose: the stamp is keyed on these, and a key computed
# before its inputs exist is a key of nothing.
cmake_args=(
  -S "$submodule" -B "$build" -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_EXTENSIONS="core_functions;icu"
  -DENABLE_EXTENSION_AUTOLOADING=0
  -DENABLE_EXTENSION_AUTOINSTALL=0
  -DDISABLE_EXTENSION_LOAD=TRUE
  -DBUILD_SHELL=0 -DBUILD_UNITTESTS=0 -DBUILD_BENCHMARKS=0
)

# Every cmake target the link needs. `duckdb_static` alone is NOT enough: the
# generated extension loader pulls in the extensions whether or not they were
# asked for, and a build without them fails at link on symbols nobody requested.
# Measured on Windows 2026-09-08; this script had only duckdb_static and would
# have produced a library that cannot be linked.
targets=(duckdb_static core_functions_extension icu_extension parquet_extension
         duckdb_generated_extension_loader)

# ── the cache stamp ──────────────────────────────────────────────────────────
#
# Keyed on the submodule commit, the flags and the compiler version — the three
# things that change the output. Nothing else, because a cache that misses for an
# irrelevant reason costs forty minutes.
#
# Correction 2026-09-08, same defect as the Windows twin carried: the key was built
# from `expected` (the list of expected LIBRARIES) plus three strings retyped by
# hand, so the actual cmake flags were never in it. Adding -DDISABLE_EXTENSION_LOAD
# would have printed "cache hit, nothing to build" and handed the old library over
# as the new one. The key now hashes "${cmake_args[@]}" itself -- the thing passed
# to cmake, not a copy of it. A copy is what broke this.
cc="${CXX:-clang++}"
commit="$(git -C "$submodule" rev-parse HEAD 2>/dev/null || echo no-submodule)"
ccver="$("$cc" --version 2>/dev/null | head -1 || echo unknown)"
key="$(printf '%s|%s|%s|%s' "$commit" "$ccver" "${cmake_args[*]}" \
       "$(IFS=,; echo "${expected[*]}")" | sha256sum | cut -d' ' -f1)"

if [ "${FORCE:-0}" != "1" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ]; then
  echo "cache hit ($key) - nothing to build. FORCE=1 to rebuild."
  exit 0
fi

mkdir -p "$lib"

echo "configure..."
cmake "${cmake_args[@]}"

echo "build (expect 40-60 min on 16 cores with a cold cache)..."
start=$(date +%s)
cmake --build "$build" --target "${targets[@]}" -j
echo "built in $((($(date +%s) - start) / 60)) min"

# ── collect and check ───────────────────────────────────────────────────────

found=()
while IFS= read -r -d '' a; do
  cp -f "$a" "$lib/"
  base="$(basename "$a")"; base="${base%.a}"; base="${base#lib}"
  found+=("$base")
done < <(find "$build" -name '*.a' -print0)

missing=()
for e in "${expected[@]}"; do
  printf '%s\n' "${found[@]}" | grep -qx "$e" || missing+=("$e")
done
extra=()
for f in "${found[@]}"; do
  printf '%s\n' "${expected[@]}" | grep -qx "$f" || extra+=("$f")
done

[ ${#missing[@]} -eq 0 ] || echo "MISSING (expected but not produced): ${missing[*]}"
[ ${#extra[@]} -eq 0 ] || echo "EXTRA (produced but not expected): ${extra[*]}"
if [ ${#missing[@]} -ne 0 ] || [ ${#extra[@]} -ne 0 ]; then
  echo "The library set changed. Update nova.toml's [ffi] libs from the manifest, dependent before dependency, and say so in the commit."
fi

: > "$manifest"
for e in "${expected[@]}"; do
  printf '%s\n' "${found[@]}" | grep -qx "$e" && echo "$e" >> "$manifest"
done
for x in ${extra[@]+"${extra[@]}"}; do echo "$x" >> "$manifest"; done

echo "$key" > "$stamp"
echo "manifest: $manifest ($(wc -l < "$manifest") libraries)"
echo "stamp:    $stamp"
