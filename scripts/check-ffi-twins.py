# -*- coding: utf-8 -*-
"""Every `extern "C" fn` in src/ffi.nv must have a prototype in native/duckdb_shim.h.

CARRIED FROM nova-sdl (2026-09-09). The history below is that package's: its pair
drifted by ten names within an hour of its header promising an eye would catch it.
This package has the identical arrangement and had no checker, while being the one
closest to a tag -- and a tag is the hardest thing here to take back.

    python scripts/check-ffi-twins.py

WHY IT EXISTS, and the answer is embarrassing in the useful way. `duckdb_shim.h` opens
with: "Every signature here has a twin in src/ffi.nv, and the two are checked against
each other by eye only. A drifted signature is not a compile error; it is a pointer
read at the wrong width, so the pair is kept short on purpose."

The pair did not stay short, and the eye did not hold. Within an hour of writing that
sentence I added ten externs for the tray and text surfaces and NONE of them to the
header -- measured 2026-09-09 19:10: 18 externs, 6 prototypes, ten names on one side
only. Nothing complained, because nothing was looking.

WHAT IT COMPARES, and what it deliberately does not. NAMES, both directions. Not
types: the two languages spell them differently (`int` against `int64_t`, `*u8`
against `const uint8_t *`), and demanding textual sameness there would make the check
noise -- and noise gets switched off, which is worse than no check. A name present on
one side only is the failure this catches, and it is the failure that actually
happened.

The types remain unchecked and that is a KNOWN HOLE, stated rather than left for
someone to discover: a signature can still drift in its argument widths and this will
pass. Closing that needs the header parsed properly, which is a bigger tool than the
problem so far deserves.
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
FFI = HERE.parent / "src" / "ffi.nv"
HDR = HERE.parent / "native" / "duckdb_shim.h"

for f in (FFI, HDR):
    if not f.exists():
        print("FFI TWINS: FAILED")
        print(f"   {f} is missing -- the pair cannot be compared, so this measured nothing")
        sys.exit(1)

ffi_src = FFI.read_text(encoding="utf-8")
hdr_src = HDR.read_text(encoding="utf-8")

# `extern "C" fn sdl_window_open(...)` -- the name is what we compare.
in_ffi = set(re.findall(r'^extern "C" fn\s+(ddb_[a-z0-9_]+)', ffi_src, re.M))

# A C prototype: any line declaring sdl_something(. Comments are stripped first, or a
# name MENTIONED in prose would count as declared -- the same "prose read as data"
# mistake this project has hit twice today.
no_block = re.sub(r"/\*.*?\*/", "", hdr_src, flags=re.S)
no_line = re.sub(r"//[^\n]*", "", no_block)
in_hdr = set(re.findall(r"\b(ddb_[a-z0-9_]+)\s*\(", no_line))

print(f"externs in ffi.nv: {len(in_ffi)}, prototypes in duckdb_shim.h: {len(in_hdr)}")

bad = []
only_ffi = sorted(in_ffi - in_hdr)
only_hdr = sorted(in_hdr - in_ffi)

if only_ffi:
    bad.append("declared in ffi.nv, NO prototype in duckdb_shim.h: " + ", ".join(only_ffi))
if only_hdr:
    bad.append("in duckdb_shim.h, no extern in ffi.nv: " + ", ".join(only_hdr) +
               " (a prototype nobody calls is dead weight, or a rename half-done)")

# Two empty sets agree about nothing. Without this the check passes loudest exactly
# when the pattern has stopped matching the language.
if not in_ffi or not in_hdr:
    bad.append("one side yielded no names at all -- the pattern has stopped matching, "
               "and a green here would mean nothing")

print("FFI TWINS:", "clean" if not bad else "FAILED")
for b in bad:
    print("  ", b)
sys.exit(1 if bad else 0)
