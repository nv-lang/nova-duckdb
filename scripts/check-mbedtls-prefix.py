#!/usr/bin/env python3
"""check-mbedtls-prefix.py -- DuckDB's private mbedTLS exports no `mbedtls_*` name.

WHY. DuckDB vendors mbedTLS 3.6.4 and nova-tls vendors 3.6.2, with different
configurations and therefore different struct layouts. Both define the same 237
external `mbedtls_*` functions. In one program the linker keeps ONE definition of
each and binds both copies to it: nothing fails to link, and a database opened with
a key under nova-tls corrupted the heap (0xC0000374, claude-limits 2026-09-30). That
is what 0.2.0 shipped.

0.2.1 builds DuckDB with native/duckdb_mbedtls_prefix.h force-included, renaming
every one of those symbols to `nova_ddb_mbedtls_*`. This check reads the built
libraries and fails on any external `mbedtls_*` name, defined OR referenced -- a
reference would bind to nova-tls just the same.

It also fails when it has nothing to read: a check that finds no libraries and
reports clean is how a missing build passes for a correct one.

REGENERATING THE HEADER (on a DuckDB upgrade that adds or removes a function): build
once WITHOUT the force-include, then
    llvm-nm --defined-only --extern-only native/lib/duckdb_mbedtls.lib
and write one `#define mbedtls_X nova_ddb_mbedtls_X` per `mbedtls_` name. The README
lists this under "What to check on EVERY DuckDB upgrade".
"""
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# An argument names another directory -- for the red probes, which must not touch
# the real build output.
LIB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "native", "lib")
PREFIX = "nova_ddb_mbedtls_"


def find_nm():
    # llvm-nm first, wherever it is: it reads MSVC archives and ELF alike, while an
    # `nm` that happens to be on a Windows PATH may be one that reads neither well.
    p = shutil.which("llvm-nm")
    if p:
        return p
    p = r"C:\Program Files\LLVM\bin\llvm-nm.exe"
    if os.path.exists(p):
        return p
    return shutil.which("nm")


def symbols(nm, path, flag):
    """External names of one archive: defined (flag --defined-only) or undefined."""
    args = [nm, flag, path] if flag == "--undefined-only" else [nm, flag, "--extern-only", path]
    out = subprocess.run(args, capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        sys.exit(f"MBEDTLS PREFIX: FAILED -- {os.path.basename(nm)} could not read {path}:\n"
                 f"{out.stderr.strip()[:400]}")
    names = []
    for line in out.stdout.splitlines():
        parts = line.split()
        if parts:
            names.append(parts[-1])
    return names


def main():
    nm = find_nm()
    if not nm:
        sys.exit("MBEDTLS PREFIX: FAILED -- neither llvm-nm nor nm found; cannot read the libraries")
    libs = sorted(glob.glob(os.path.join(LIB, "*.lib")) + glob.glob(os.path.join(LIB, "*.a")))
    if not libs:
        sys.exit(f"MBEDTLS PREFIX: FAILED -- no libraries in {LIB}; build them first "
                 f"(scripts/build-duckdb.*) -- an empty directory is not a clean one")
    bad = []
    prefixed = 0
    for path in libs:
        name = os.path.basename(path)
        for flag in ("--defined-only", "--undefined-only"):
            for s in symbols(nm, path, flag):
                # MSVC x64 has no leading underscore; ELF has none either. Strip one
                # anyway so a 32-bit or Mach-O archive is not read as clean.
                bare = s[1:] if s.startswith("_mbedtls_") else s
                if bare.startswith("mbedtls_"):
                    bad.append(f"{name}: {'defines' if flag == '--defined-only' else 'references'} {bare}")
                elif bare.startswith(PREFIX) and flag == "--defined-only":
                    prefixed += 1
    print(f"libraries read: {len(libs)}; defined {PREFIX}* symbols: {prefixed}")
    if bad:
        print(f"MBEDTLS PREFIX: FAILED -- {len(bad)} unprefixed mbedTLS symbol(s):")
        for b in bad[:40]:
            print("   " + b)
        if len(bad) > 40:
            print(f"   ... and {len(bad) - 40} more")
        print("   DuckDB's mbedTLS will bind to nova-tls's copy in any program that has both.")
        sys.exit(1)
    # The control: zero prefixed symbols means the force-include never reached the
    # compiler (or mbedTLS left the build), and "no unprefixed name" is then vacuous.
    if prefixed == 0:
        sys.exit("MBEDTLS PREFIX: FAILED -- not one prefixed symbol either; the prefix "
                 "header did not reach the build, so a clean result would prove nothing")
    print("MBEDTLS PREFIX: clean")


main()
