# -*- coding: utf-8 -*-
"""Does `[ffi] libs` in nova.toml still match what the build actually produced?

    python scripts/check-libs-manifest.py

WHY IT EXISTS. nova.toml said, in a comment, that "a test compares the two (01.4
sec.8)". No such test existed -- a claim with no mechanism, which is the same shape
as a guard that reports an acceptance it never measured. Written 2026-09-08 to make
the sentence true.

WHAT IT COMPARES, and why order is part of it. `scripts/build-duckdb.*` writes the
libraries it produced into `scripts/libs-manifest.txt`, dependent before dependency,
and `[ffi] libs` is filled from that file BY HAND on a DuckDB version bump. A hand
copy is exactly the step that goes stale, and 01.4 sec.8 names the risk it goes stale
FOR: the vendored dependency set changes between DuckDB versions. Order is compared
too, not just membership -- a static link resolves left to right, so the same names
in the wrong order is a link error, not a cosmetic difference.

MISSING MANIFEST IS A FAILURE, NOT A SKIP. If the build has not run there is nothing
to compare against, and the claim in nova.toml is unverified. Saying "clean" then
would be the precise thing this checker was written to stop. It exits non-zero and
names the command that produces the file.
"""
import pathlib
import re
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "scripts" / "libs-manifest.txt"
TOML = ROOT / "nova.toml"


# Supplied by the operating system, never by the build. Kept here rather than guessed
# at (say, "anything without a duckdb_ prefix") because a guess would also swallow a
# real vendored library whose name happens not to match, and that is precisely the
# drift this checker exists to catch.
SYSTEM_LIBS = {"rstrtmgr", "ws2_32", "bcrypt", "advapi32", "ole32", "shell32",
               "kernel32", "user32", "crypt32", "secur32", "wldap32", "normaliz"}


def main():
    if not TOML.exists():
        print(f"FAILED: {TOML} does not exist")
        return 1
    with TOML.open("rb") as f:
        declared = ((tomllib.load(f).get("ffi") or {}).get("libs")) or []
    if not declared:
        print("FAILED: nova.toml has no [ffi] libs -- nothing is declared to link")
        return 1

    if not MANIFEST.exists():
        print(f"libs declared in nova.toml: {len(declared)}")
        print("LIBS MANIFEST: NOT COMPARED -- scripts/libs-manifest.txt does not exist.")
        print("  The build has not run here, so the list in nova.toml is UNVERIFIED.")
        print("  Produce it with:  pwsh -File scripts/build-duckdb.ps1   (or ./scripts/build-duckdb.sh)")
        print("  This is a failure and not a skip on purpose: a checker that reports")
        print("  success when it compared nothing is the defect it exists to catch.")
        return 1

    # `utf-8-sig` and an explicit strip of the mark: PowerShell 5.1 writes UTF-8 WITH
    # a byte-order mark, and on the first real build that made this checker report
    # `core_functions_extension` as BOTH missing and extra -- the signature of two
    # strings that look identical and are not. A reader acting on that would have
    # edited the manifest in precisely the wrong direction. A checker defeated by an
    # invisible character does not report "unknown"; it reports something false.
    text = MANIFEST.read_text(encoding="utf-8-sig")
    built = [l.strip().lstrip("\ufeff") for l in text.splitlines() if l.strip()]

    from_build = [x for x in declared if x not in SYSTEM_LIBS]
    print(f"declared in nova.toml: {len(declared)} "
          f"({len(declared) - len(from_build)} system-supplied), "
          f"built per the manifest: {len(built)}")

    bad = []
    missing = [x for x in built if x not in declared]
    extra = [x for x in from_build if x not in built]
    if missing:
        bad.append(f"produced by the build but NOT in [ffi] libs: {missing} "
                   f"-- the link will not find them")
    if extra:
        bad.append(f"in [ffi] libs but NOT produced: {extra} "
                   f"-- a vendored dependency vanished between DuckDB versions (01.4 sec.8)")
    if not missing and not extra and from_build != built:
        # Same names, different sequence. A static link resolves left to right.
        first = next(i for i, (a, b) in enumerate(zip(from_build, built)) if a != b)
        bad.append(f"same libraries, DIFFERENT ORDER, first difference at position {first}: "
                   f"nova.toml has {from_build[first]!r}, the manifest has {built[first]!r}. "
                   f"Order is dependent-before-dependency and a static link honours it.")

    print("LIBS MANIFEST:", "clean" if not bad else "FAILED")
    for b in bad:
        print("  ", b)
        print("   Fix: copy the manifest into [ffi] libs in that order, and say so in the commit.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
