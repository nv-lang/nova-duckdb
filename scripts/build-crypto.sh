#!/usr/bin/env bash
# build-crypto.sh -- the OS-nonce AES-GCM library (native/os_nonce_crypto.cpp).
#
# Linux twin of build-crypto.ps1 -- the reasons are written there. Called by
# build-duckdb.sh on EVERY run, the cache hit included. Built with the compiler the
# DuckDB build uses here (CXX, clang++ by default) so the C++ ABI matches
# libduckdb_static.a.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
work="$root/native/build/crypto"
lib="$root/native/lib"
cxx="${CXX:-clang++}"
mkdir -p "$work" "$lib"
echo "crypto: compiling $root/native/os_nonce_crypto.cpp"
# The same symbol prefix DuckDB's mbedTLS is built with -- see build-duckdb.sh.
"$cxx" -c -O2 -std=c++17 -fPIC -DNDEBUG -DDUCKDB_STATIC_BUILD \
  -include "$root/native/duckdb_mbedtls_prefix.h" \
  -I "$root/native/duckdb/src/include" \
  -I "$root/native/duckdb/third_party/mbedtls/include" \
  "$root/native/os_nonce_crypto.cpp" -o "$work/os_nonce_crypto.o"
ar rcs "$lib/libnova_duckdb_crypto.a" "$work/os_nonce_crypto.o"
echo "crypto: $lib/libnova_duckdb_crypto.a"
