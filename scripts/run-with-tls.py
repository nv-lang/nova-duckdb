#!/usr/bin/env python3
"""run-with-tls.py -- build and run examples/03-with-tls: DuckDB with a key + nova-tls.

The behaviour behind check-mbedtls-prefix.py. That check reads symbol tables; this
one links nova-duckdb and nova-tls into ONE program, opens a database with a key,
writes, reopens, reads, and parses TLS roots before and after. Under 0.2.0 such a
program either failed to link (duplicate `mbedtls_*`) or, with another link order,
linked and corrupted the heap at the first encrypted write (claude-limits,
2026-09-30). It is not in the CI guard loop: Linux CI cannot link C++ yet (see the
KNOWN BLOCKER in pkg-gate.yml), so this runs where the package tests run -- before
every tag, on Windows.

    python scripts/run-with-tls.py
    NOVA=path/to/nova NOVA_TLS_PATH=../nova-tls python scripts/run-with-tls.py

Without an existing examples/03-with-tls/nova.override.toml it writes one pointing
`duckdb` at this working copy (and `tls` at NOVA_TLS_PATH when set, else the tag),
and removes it afterwards. An existing override is used as it is and left alone.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(os.path.dirname(HERE), "examples", "03-with-tls")
OVERRIDE = os.path.join(EXAMPLE, "nova.override.toml")
MARK = "with-tls: OK"


def main():
    nova = os.environ.get("NOVA", "nova")
    made = False
    if not os.path.exists(OVERRIDE):
        lines = ["[replace]", 'duckdb = { path = "../.." }']
        tls = os.environ.get("NOVA_TLS_PATH")
        if tls:
            lines.append('tls = { path = "%s" }' % os.path.abspath(tls).replace("\\", "/"))
        with open(OVERRIDE, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        made = True
    try:
        cmd = nova.split() + ["test", "with_tls.nv", "--verbose"]
        p = subprocess.run(cmd, cwd=EXAMPLE, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=900)
    finally:
        if made:
            os.remove(OVERRIDE)
    out = p.stdout + p.stderr
    for line in out.splitlines():
        if "roots:" in line or "with_tls" in line or "SUMMARY" in line or "PASS:" in line:
            print(line.strip()[:300])
    # Three things, not the exit code alone: the runner passed ONE test, failed none,
    # and the program itself reached its last line. A green summary of zero tests is
    # how a program that never ran passes.
    ok = p.returncode == 0 and "PASS: 1" in out and "FAIL: 0" in out and MARK in out
    if not ok:
        print("WITH TLS: FAILED -- nova-duckdb with a key and nova-tls do not run together")
        if p.returncode != 0:
            print(out[-3000:])
        sys.exit(1)
    print("WITH TLS: clean")


main()
