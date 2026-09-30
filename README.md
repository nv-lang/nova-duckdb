# nova-duckdb

DuckDB for Nova: real column types, an Appender for bulk writes, and the
`core_functions` and `icu` extensions linked in statically so nothing reaches the
network at runtime.

**This is not a complete binding of DuckDB.** Version 0.1 gives Nova a finished core
plus what its first consumer needs; the rest arrives minor version by minor version
as consumers appear, *without* rebuilding the library — the static archive already
contains everything. A request to widen the surface is an issue naming the consumer
and the function, not a "while we are here".

**The tag is load-bearing, whatever the version is called.** A consumer pulls this
package with `version = "0.1"`, and the resolver takes the newest matching **tag** --
so a fix living on `main` reaches nobody until it is tagged, and the next tag moves
every consumer's build with no announcement and nothing for them to do. 0.1.0 is not
declared a release; it still behaves like one for anybody who depends on it.

## Status

| | |
|---|---|
| Nova surface | written, `nova check` clean |
| C shim | compiled against the vendored DuckDB 1.5.5 — see [Building](#building) |
| Tests | run where DuckDB is built (`nova test src`: PASS, 2026-09-30) |
| Tag | `v0.2.0` — encryption, and the `.of` constructors renamed `.new` (breaking); `v0.1.1` for consumers still on 0.1 |

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
and hangs for about twenty-two seconds before failing.

**How the network is actually kept out, and what was not enough.** The shim sets
`autoinstall_known_extensions=false` and `autoload_known_extensions=false` at every
connection, and the build passes the cmake flags that make those the defaults. That
stops DuckDB going for an extension *by itself* — and it is all it stops. An explicit
`INSTALL json` is not automatic, it is a direct request, and it ran: measured
2026-09-08, it fetched 22 MB from `http://extensions.duckdb.org` — over plain HTTP,
no TLS — and loaded it into the process.

The library is therefore built with `-DDISABLE_EXTENSION_LOAD`, which removes
installing and dynamic loading at compile time. `INSTALL` now answers
`Installing external extensions is disabled through a compile time flag`, and no SQL
can undo a build flag. The statically linked extensions are unaffected: they are
registered at startup by a different path, and the test that proves `icu` works is
the control for it.

A test in `src/duckdb_test.nv` asserts that specific refusal message rather than
merely that some error occurred -- a test satisfied by any error is satisfied by the
wrong one, which is how two other tests in this file were green while measuring
nothing. Whether `~/.duckdb` gets created is checked by hand at present, not by the
suite; the test file's banner claimed otherwise and has been corrected.

Adds about **36 MB** to a binary on each platform.

## Encryption (0.2)

`DbOptions.encryption_key` non-empty opens the FILE encrypted: AES-GCM, the key
derived by DuckDB from the string you pass. The same key reads it back; any other key,
or none, is refused with DuckDB's own message ("... wrong encryption key ..."). The
bytes on disk carry no plaintext -- `src/encryption_test.nv` greps a written row and,
as a control, finds it in an unencrypted file.

**How.** DuckDB 1.5.5 refuses to WRITE an encrypted file with the crypto it carries:
its AES-GCM is mbedTLS, but its nonce source is a `RandomEngine` (PCG), and a nonce
repeated under one key breaks GCM. It asks for httpfs (OpenSSL and a network file
system -- not in a package that must stay off the network) or `force_mbedtls_unsafe`.
`native/os_nonce_crypto.cpp` keeps DuckDB's AES-GCM and replaces only the random
source -- `BCryptGenRandom` on Windows, `getrandom` on Linux -- and installs it as the
database's `DBConfig::encryption_util`. The shim then opens an in-memory database,
`ATTACH`es the file with the key, and runs `USE` on every connection.

It is built as its own static library, `nova_duckdb_crypto`, by
`scripts/build-crypto.{ps1,sh}` (called by `build-duckdb.*` on every run) with the
compiler and flags DuckDB was built with: `[ffi] c_shims` compiles C only, and the file
uses DuckDB's C++ classes.

Not from the OS: the database identifier and key ids DuckDB draws from `RandomEngine`
(`GenerateDBIdentifier`, `GenerateRandomKeyID`). They are salts and names, which need to
differ, not to be unpredictable; every nonce and every temporary key goes through the
replaced method.

The key passes through SQL text once, inside the shim (`ATTACH` takes no bound
parameters). The statement buffer is wiped before it is freed, and a DuckDB error
message that contains the key is replaced by one that does not.

### What to check on EVERY DuckDB upgrade

This reaches into DuckDB's internals. Before bumping the submodule, check each of these
against the new version, and run `src/encryption_test.nv`:

1. `duckdb::DatabaseWrapper` in `src/include/duckdb/main/capi/capi_internal.hpp` still
   holds `shared_ptr<DuckDB> database` (how a `duckdb_database` is reached).
2. `DBConfig::encryption_util` in `src/include/duckdb/main/config.hpp` is still a
   `shared_ptr<EncryptionUtil>`, and `DatabaseInstance::GetEncryptionUtil`
   (`src/main/database.cpp`) still returns it before it would refuse.
3. `EncryptionUtil::CreateEncryptionState(unique_ptr<EncryptionStateMetadata>)` and
   `EncryptionState::GenerateRandomData(data_ptr_t, idx_t)` keep their signatures
   (`src/include/duckdb/common/encryption_state.hpp`).
4. `duckdb_mbedtls::MbedTlsWrapper::AESStateMBEDTLS` still has a public constructor
   taking the metadata (`third_party/mbedtls/include/mbedtls_wrapper.hpp`).
5. Every call site that draws nonces or temporary keys still goes through the state's
   `GenerateRandomData`, not a static or a `RandomEngine`:
   `grep -rn "GenerateRandomData\|RandomEngine" src/storage src/common/encryption*`.
6. `ATTACH ... (ENCRYPTION_KEY ...)` is still how a file is opened encrypted.

A changed signature fails the build; a changed CALL SITE (5) does not -- it would
silently put `RandomEngine` nonces back. That is why it is on this list by grep.

## 0.2.0: what changed for a consumer

* **Breaking:** the one-argument constructors are `.new`, not `.of` --
  `DbRef.new`, `ConnRef.new`, `ResultRef.new`, `DecimalValue.new`. `of` is reserved for
  variadic collections (`W_NONVARIADIC_OF`).
* `open` with a non-empty `encryption_key` now opens the file encrypted instead of
  refusing.
* Links one more archive, `nova_duckdb_crypto`, first in `[ffi] libs`.

## What it costs you to know

* **Row-at-a-time writes are slow.** About two milliseconds per `INSERT` — DuckDB is
  a columnar engine and this is not its path. Use the Appender; `exec` is for
  targeted `UPDATE`s.
* **A connection is thread-affine.** Every extern is `#thread_affine`, so calling one
  from inside a `spawn` is a compile error rather than a race found in production.
* **There are TWO layers, and they give different guarantees.** The `Db` effect
  (`db_effect.nv`) is the mockable one: nine operations, a real handler over DuckDB
  (`db_real.nv`) and any handler you write. The facade (`Database`/`Connection`/
  `QueryResult`) calls the C shim directly and needs no handler.
* **THE EFFECT COVERS 11 OF THE FACADE'S 60 SHIM CALLS, AND THAT IS AN INTERMEDIATE
  STATE RATHER THAN THE DESIGN.** Value readers, the appender and prepared statements
  go past the effect today. This package's `Db` was written before a cross-driver
  boundary existed; plan 286 defines one and puts row reading INSIDE the effect
  (`Rows.@next()`), so the slice you see here is where this package got to, not where
  it is meant to stop. Its stage D5 brings that boundary here **without changing this
  package's public API** -- no version is promised, because the stage carries an open
  question of its own.
* **What follows from it TODAY, and it is the thing to know: a mock substitutes the
  lifecycle and NOT the reading.** Mock `query`, then call `value()`, and a fake
  handle reaches a real C call. (The shape of the un-covered calls is also what D456
  п.3 forbids across an effect boundary -- `ddb_value_i64(result, row, col)` walks by
  index -- which is why the boundary that does cover them is a redesign rather than
  more operations of the same kind.)
* **So: to test against a substitute, use the EFFECT, not the facade.** `Db.describe`
  and `Db.next` read rows in one operation each, which is what D456 prescribes and
  what a mock can honestly serve. The facade offers the same pair (`describe`,
  `next_row`) for direct use.
* **A handler is required at RUN time, not at compile time.** Calling an effect
  operation with no `with Db = ...` in scope compiles and then fails at runtime with
  `unhandled effect Db.<op>: no active handler`. Measured, not assumed -- the same is
  true of any effect in this language, so it is the normal shape rather than a
  surprise this package invented.

* **Encryption and `read_only` are not implemented in 0.1** and are *refused* rather
  than ignored, because a security setting that silently does nothing is worse than
  an error.

## Licence

`MIT OR Apache-2.0`. DuckDB itself is MIT; see its own repository under
`native/duckdb`.
