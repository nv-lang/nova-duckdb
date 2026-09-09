# -*- coding: utf-8 -*-
"""The two build scripts must agree, and this is what checks that they do.

    python scripts/check-build-twins.py

WHY IT EXISTS. `build-duckdb.ps1` and `build-duckdb.sh` build the same library on two
platforms, and their shared header has always said so: "The Windows twin is
build-duckdb.ps1 and the two must stay in step: same flags, same cache key, same
manifest."

Saying it was not enough. On 2026-09-08 they diverged THREE TIMES in one night, every
time because I edited one and not the other:

  1. -DDISABLE_EXTENSION_LOAD, the flag that stops the library reaching the network,
     went into the PowerShell twin only -- so the security fix was Windows-only;
  2. the cache key was rebuilt to hash the real cmake flags in one and not the other;
  3. the `expected` library list gained three entries in one and not the other, which
     surfaced on CI as "same libraries, DIFFERENT ORDER at position 14" -- the three
     missing names fell through to the tail where extras are appended.

Only the third was caught by anything, and only because pkg-gate had just started
running on Linux. The first two would have sat there.

WHAT IS COMPARED, and what deliberately is not. The cmake flags and the expected
library list -- the two things that decide what comes out of the build. NOT the prose,
NOT the shell idioms, NOT the order of the sections: they are different languages and
demanding textual sameness would make the check noise, and noise gets switched off.
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
PS1 = HERE / "build-duckdb.ps1"
SH = HERE / "build-duckdb.sh"


def die(msg):
    print("BUILD TWINS: FAILED")
    print("  ", msg)
    sys.exit(1)


for f in (PS1, SH):
    if not f.exists():
        die(f"{f.name} is missing -- the twins cannot be compared, so this check "
            f"measured nothing")

ps = PS1.read_text(encoding="utf-8")
sh = SH.read_text(encoding="utf-8")


# Flags that belong to ONE platform and must not be demanded of the other. Each needs
# a reason, because an unexplained entry here is how a real divergence gets hidden.
PLATFORM_ONLY = {
    # /MT, so DuckDB uses the same C runtime as the Nova runtime does. There is no
    # MSVC runtime to select on Linux.
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded": "ps1",
}


def flags(text):
    """Every -DNAME=VALUE the script hands to cmake, as a set.

    Quotes are stripped from the value: the bash twin writes
    -DBUILD_EXTENSIONS="core_functions;icu" and the PowerShell one writes it bare, so
    comparing them raw reports a difference that is only punctuation. The first run of
    this check said exactly that, which is the sort of noise that gets a guard turned
    off rather than fixed.
    """
    out = set()
    for m in re.finditer(r'-D([A-Z_0-9]+)=("[^"]*"|\'[^\']*\'|[^\s,)]+)', text):
        name, val = m.group(1), m.group(2).strip('"\'')
        out.add(f"-D{name}={val}")
    return out


def expected_ps():
    m = re.search(r"\$expected = @\(\n(.*?)\n\)", ps, re.S)
    if not m:
        die("cannot find $expected in build-duckdb.ps1")
    body = re.sub(r"#[^\n]*", "", m.group(1))
    return [w.strip().strip('",') for w in re.split(r",\s*|\n", body) if w.strip().strip('",')]


def expected_sh():
    m = re.search(r"^expected=\(\n(.*?)\n\)", sh, re.S | re.M)
    if not m:
        die("cannot find expected=( in build-duckdb.sh")
    body = re.sub(r"#[^\n]*", "", m.group(1))
    return [w for w in re.split(r"\s+", body) if w]


bad = []

# --- 1. the cmake flags ------------------------------------------------------
fp, fs = flags(ps), flags(sh)
only_ps = sorted(f for f in fp - fs if PLATFORM_ONLY.get(f) != "ps1")
only_sh = sorted(f for f in fs - fp if PLATFORM_ONLY.get(f) != "sh")
allowed = sorted(f for f in (fp ^ fs) if f in PLATFORM_ONLY)
print(f"cmake flags: ps1 {len(fp)}, sh {len(fs)}")
if allowed:
    print(f"  platform-only, allowed: {allowed}")
if only_ps:
    bad.append(f"flags only in build-duckdb.ps1: {only_ps}")
if only_sh:
    bad.append(f"flags only in build-duckdb.sh: {only_sh}")

# An exception that no longer applies is worse than none: it silently permits a
# divergence that has already been fixed elsewhere.
for f, side in sorted(PLATFORM_ONLY.items()):
    present = (f in fp) if side == "ps1" else (f in fs)
    if not present:
        bad.append(f"stale exception: {f} is allowed on {side} but is not there any "
                   f"more -- remove the entry or restore the flag")

# A comparison of two empty sets is not agreement.
if not fp or not fs:
    bad.append("one of the scripts yielded no cmake flags at all -- the pattern has "
               "stopped matching, and this check would pass whatever they contained")

# --- 2. the expected library list, IN ORDER ----------------------------------
ep, es = expected_ps(), expected_sh()
print(f"expected libraries: ps1 {len(ep)}, sh {len(es)}")
if not ep or not es:
    bad.append("one of the expected lists parsed to nothing")
elif ep != es:
    if set(ep) == set(es):
        first = next(i for i, (a, b) in enumerate(zip(ep, es), 1) if a != b)
        bad.append(f"same libraries, different ORDER, first difference at {first}: "
                   f"ps1 has {ep[first-1]!r}, sh has {es[first-1]!r}. The order is the "
                   f"link order and the manifest is written in it.")
    else:
        bad.append(f"only in ps1: {sorted(set(ep) - set(es))}; "
                   f"only in sh: {sorted(set(es) - set(ep))}")

print("BUILD TWINS:", "clean" if not bad else "FAILED")
for b in bad:
    print("  ", b)
sys.exit(1 if bad else 0)
