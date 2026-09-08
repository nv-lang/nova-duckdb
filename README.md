# nova-duckdb

DuckDB for Nova: real column types, an Appender for bulk writes, and the
`core_functions` and `icu` extensions linked in statically so nothing reaches the
network at runtime.

**This is not a complete binding of DuckDB.** Version 0.1 gives Nova a finished core
plus what its first consumer needs; the rest arrives minor version by minor version
as consumers appear, *without* rebuilding the library — the static archive already
contains everything. A request to widen the surface is an issue naming the consumer
and the function, not a "while we are here".

## Status

| | |
|---|---|
| Nova surface | written, `nova check` clean |
| C shim | written, **never compiled** — see [Building](#building) |
| Tests | written, **never run** |
| Tag | none yet |

The package cannot be built where DuckDB has not been built first, and on a fresh
checkout `nova test src` reports `CC-FAIL` on `src/duckdb_test` with
`duckdb.h: file not found`. That is expected until the submodule is present and
`scripts/build-duckdb.*` has run. Nothing here is claimed as passing.

## Using it

```nova
import duckdb.{Database, DbOptions, DuckError}
import duckdb.types.{Value}

fn main() Io -> () {
    with Fail[DuckError] = |e| { print("duckdb: ${e.message()}") } {
        consume db = Database.open("limits.duckdb", DbOptions.defaults()) {
            consume con = db.connect() {
                ro _ = con.exec("CREATE TABLE sample (at TIMESTAMP, percent TINYINT)")

                // The Appender is the only fast way in: a row at a time through
                // INSERT costs about two milliseconds, a hundred thousand appended
                // rows cost about ten milliseconds in total.
                consume app = con.appender("", "sample") {
                    app.row([Value.Instant(now), Value.Int(74)])
                    app.flush()
                }

                consume r = con.query("SELECT count(*) FROM sample") {
                    print("rows: ${r.value(0, 0)}")
                }
            }
        }
    }
}
```

Every handle is a `consume` type with a `@cleanup`, so `consume x = … { … }` ties it
to a block and using one after it closes is a **compile error**, not a
use-after-free. No exported signature in this package is `unsafe` and no consumer
ever holds a raw handle.

For the `sql`…`` tag from `std/data/sql.nv`:

```nova
import duckdb.sqltag
consume r = con.run(sql`SELECT * FROM sample WHERE percent > ${threshold}`) { … }
```

A value passed that way never becomes part of the statement text, so injection is
not a thing that can happen.

## Types

| DuckDB | Nova | note |
|---|---|---|
| `BOOLEAN` | `bool` | |
| `TINYINT` … `BIGINT` | `Value.Int(i64)` | |
| `UTINYINT` … `UBIGINT` | `Value.UInt(u64)` | |
| `HUGEINT` | `Value.Huge(i64, u64)` | two halves; the C ABI for `__int128` is not portable across the three toolchains |
| `FLOAT`, `DOUBLE` | `Value.Real(f64)` | |
| `DECIMAL(p,s)` | `Value.Dec(DecimalValue)` | **never folded into a float**, not even to print |
| `VARCHAR`, `ENUM` | `Value.Text(str)` | |
| `BLOB` | `Value.Bytes([]u8)` | a separate accessor from text: the text one *converts* |
| `TIMESTAMP`, `TIMESTAMPTZ` | `Value.Instant(Timestamp)` | µs ↔ ns; sub-microsecond precision is **refused**, not truncated |
| `DATE` | `Value.Day(Date)` | |
| `TIME` | `Value.TimeOfDay(Duration)` | since midnight |
| `INTERVAL` | `Value.Span(Duration)` | an interval carrying **months** is refused: a month is not a fixed number of days |
| `UUID` | `Value.Ident(Uuid)` | 16 bytes, no string parsing |
| `LIST`, `STRUCT`, `MAP`, `ARRAY`, `UNION`, `BIT` | — | `DuckError.Type` naming the column |

Two rules are worth stating on their own, because both protect against a value that
reads back *different* from what was written:

* **A `DECIMAL` is never a float.** A percentage stored as `DECIMAL(5,2)` is an exact
  number someone compares against a threshold; a float is how 74.00 becomes
  73.999999.
* **An instant finer than a microsecond is refused.** DuckDB counts microseconds. A
  truncated write is a different moment, and a chart drawn from it is wrong in a way
  nobody can see.

## Building

```sh
git submodule update --init --depth 1 native/duckdb
./scripts/build-duckdb.sh            # or: pwsh -File scripts/build-duckdb.ps1        # PowerShell 7; where it is absent:
powershell -ExecutionPolicy Bypass -File scripts\build-duckdb.ps1
nova test src
```

Requirements: **CMake ≥ 3.20, Ninja**, clang (clang-cl under `vcvars64` on Windows),
8 GB RAM. Expect **40–60 minutes on 16 cores** with a cold cache; the result is
cached by a stamp keyed on the submodule commit, the flags and the compiler version,
so a second run is a no-op.

The repository is built rather than the amalgamation, and that is the whole reason
this takes an hour: `core_functions` and `icu` are not in `libduckdb-src.zip`, and
`date_trunc`, `time_bucket` and `TIMESTAMPTZ` arithmetic live there. With extension
autoloading on and the extension missing, the first such call goes to the internet
and hangs for about twenty-two seconds before failing. The shim turns autoloading
off at the connection and the build turns it off in the library, so that failure
cannot come back.

Adds about **36 MB** to a binary on each platform.

## What it costs you to know

* **Row-at-a-time writes are slow.** About two milliseconds per `INSERT` — DuckDB is
  a columnar engine and this is not its path. Use the Appender; `exec` is for
  targeted `UPDATE`s.
* **A connection is thread-affine.** Every extern is `#thread_affine`, so calling one
  from inside a `spawn` is a compile error rather than a race found in production.
* **Encryption and `read_only` are not implemented in 0.1** and are *refused* rather
  than ignored, because a security setting that silently does nothing is worse than
  an error.

## Licence

`MIT OR Apache-2.0`. DuckDB itself is MIT; see its own repository under
`native/duckdb`.
