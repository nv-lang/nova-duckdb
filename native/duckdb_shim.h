/* duckdb_shim.h — flat C wrappers over the DuckDB C API, for Nova's FFI.
 *
 * Source: subplan 01.4 §3 of claude-limits.
 *
 * ── Two rules this header exists to enforce ─────────────────────────────────
 *
 * 1. DUCKDB_STATIC_BUILD IS DEFINED BEFORE duckdb.h, ALWAYS. Without it the C API
 *    is declared `__declspec(dllimport)` on Windows and lld-link refuses to take
 *    the symbols out of the static archive. The spike of 2026-09-06 lost an hour
 *    to this; it is defined here rather than in the build script so that a
 *    consumer compiling the shim by any route gets it.
 *
 * 2. NO LOGIC LIVES HERE. Every function is a wrapper: arguments in, a result
 *    code out, the payload through an out-pointer. Anything that decides
 *    something belongs in Nova, where it can be read and tested. The one
 *    exception is `ddb_open`, which applies the five settings of §3 that must
 *    not be reachable from Nova at all -- see its comment.
 *
 * ── Handles come back as the RETURN VALUE ───────────────────────────────────
 *
 * A call that produces a handle returns it (NULL on failure) and reports the code
 * through an `int *out_err`. Not the other way round, and the reason is on the Nova
 * side: an out-pointer for a handle needs a pre-initialised variable of a
 * pointer-newtype, and Nova has no null literal for one. An `int` initialises as 0
 * and needs nothing. `std/net/ffi.nv` shapes `net_tcp_listen`/`accept`/`connect`
 * exactly this way.
 *
 * ── The shape of every function ─────────────────────────────────────────────
 *
 * Return value is `int`: 0 on success, non-zero on failure. The message of a
 * failure is fetched separately with `ddb_last_error`, which returns a pointer
 * valid until the next call on the same handle. Handles are opaque `void*`
 * allocated here; Nova wraps them in `consume` newtypes so that use-after-close
 * is a compile error rather than a crash.
 *
 * Strings cross as (pointer, length) pairs, never as NUL-terminated C strings:
 * Nova's `str` is not NUL-terminated and a length is what it actually has.
 * Timestamps cross as int64 microseconds UTC -- DuckDB's own unit. Decimals cross
 * as (int64 hi, uint64 lo, uint8 scale): a 128-bit integer split, because the C
 * ABI for __int128 is not something to rely on across three toolchains.
 */

#ifndef NOVA_DUCKDB_SHIM_H
#define NOVA_DUCKDB_SHIM_H

/* Before any DuckDB header. See rule 1 above. */
#ifndef DUCKDB_STATIC_BUILD
#define DUCKDB_STATIC_BUILD 1
#endif

#ifdef _WIN32
/* `windows.h` defines a macro called `interface`, and DuckDB uses that word as an
 * identifier; NOMINMAX keeps its min/max macros out of C++ headers. Both are
 * mandatory and both were found by the spike. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
/* DuckDB pulls in the Restart Manager, Winsock and bcrypt on Windows. Declaring
 * them here means `[ffi] libs` does not have to carry platform-specific entries. */
#pragma comment(lib, "rstrtmgr")
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── error codes ────────────────────────────────────────────────────────────
 *
 * Deliberately few. Nova turns these into `DuckError`, whose variants match one
 * for one; a code that means nothing to Nova would be a code Nova has to guess
 * about. */
#define DDB_OK        0
#define DDB_E_OPEN    1
#define DDB_E_QUERY   2
#define DDB_E_PREPARE 3
#define DDB_E_BIND    4
#define DDB_E_APPEND  5
#define DDB_E_TYPE    6
#define DDB_E_CLOSED  7

/* ── column types ───────────────────────────────────────────────────────────
 *
 * THE SHIM'S OWN CODES, NOT DUCKDB'S. `ddb_result_col_type` translates
 * `DUCKDB_TYPE_*` into this set, and Nova only ever sees this set.
 *
 * Two reasons, and the second is the one that matters. First: DuckDB's enum is
 * defined in `duckdb.h`, so a Nova-side copy of its numbers is a copy that has to
 * be kept in step by hand. Second: the translation lives in the file that
 * `#include`s the header, which means a DuckDB version that renumbers or adds a
 * type produces a COMPILE error in the shim's `switch` -- not a column silently
 * read as the wrong shape.
 *
 * `DDB_T_UNSUPPORTED` is deliberate and is not an error code: LIST, STRUCT, MAP,
 * ARRAY, UNION and BIT are out of scope for 0.1 (01.4 §4.1), and Nova turns this
 * into `DuckError.Type` naming the column, which is a better answer than a
 * mis-read value.
 */
#define DDB_T_UNSUPPORTED  0
#define DDB_T_BOOLEAN      1
#define DDB_T_TINYINT      2
#define DDB_T_SMALLINT     3
#define DDB_T_INTEGER      4
#define DDB_T_BIGINT       5
#define DDB_T_UTINYINT     6
#define DDB_T_USMALLINT    7
#define DDB_T_UINTEGER     8
#define DDB_T_UBIGINT      9
#define DDB_T_HUGEINT     10
#define DDB_T_FLOAT       11
#define DDB_T_DOUBLE      12
#define DDB_T_DECIMAL     13
#define DDB_T_VARCHAR     14
#define DDB_T_BLOB        15
#define DDB_T_TIMESTAMP   16
#define DDB_T_TIMESTAMPTZ 17
#define DDB_T_DATE        18
#define DDB_T_TIME        19
#define DDB_T_INTERVAL    20
#define DDB_T_ENUM        21
#define DDB_T_UUID        22
#define DDB_T_SQLNULL     23

/* ── database and connection ────────────────────────────────────────────────*/

/* Open a database file, or ":memory:".
 *
 * THIS IS THE ONE FUNCTION WITH LOGIC IN IT, and the logic is a list of settings
 * that must not be reachable from Nova (§3):
 *
 *     SET autoinstall_known_extensions = false;
 *     SET autoload_known_extensions   = false;
 *     SET temp_directory = '<temp_dir>';
 *     SET TimeZone = 'UTC';
 *     SET memory_limit = '<memory_limit>';
 *     SET threads = <threads>;
 *
 * The first two are why they live here. With autoloading on, the first call to
 * `date_trunc` sends DuckDB to the internet for an extension and hangs the process
 * for twenty-two seconds before failing -- measured in the spike. A binary that is
 * supposed to work offline cannot leave that switch where a caller might not flip
 * it, so no Nova code is given the chance.
 *
 * `temp_dir` empty means the database's own directory; it is never `~/.duckdb`. */
void *ddb_open(const uint8_t *path, int path_len,
               const uint8_t *temp_dir, int temp_dir_len,
               const uint8_t *memory_limit, int memory_limit_len,
               int threads, int read_only,
               const uint8_t *encryption_key, int encryption_key_len,
               int *out_err);

void *ddb_connect(void *db, int *out_err);
void ddb_disconnect(void *conn);
void ddb_close(void *db);

/* The DuckDB library version, e.g. "v1.5.5". Points into static storage. */
const char *ddb_version(void);

/* ── errors ─────────────────────────────────────────────────────────────────
 *
 * The message of the last failure on this handle. Valid until the next call on
 * the same handle; Nova copies it immediately. `kind` selects which handle the
 * message belongs to, because DuckDB keeps them separately: 0 database,
 * 1 connection, 2 result, 3 prepared statement, 4 appender. */
const uint8_t *ddb_last_error(void *handle, int kind, int *out_len);

/* ── queries ────────────────────────────────────────────────────────────────*/

/* Run a statement, discard any result, report rows changed. */
int ddb_exec(void *conn, const uint8_t *sql, int sql_len, int64_t *out_changed);

/* Run a query and materialise the whole result. Version 0.1 does not stream
 * (01.4 §4.1) -- a chunked reader is added when a consumer needs one. */
void *ddb_query(void *conn, const uint8_t *sql, int sql_len, int *out_err);

void ddb_result_free(void *result);
int64_t ddb_result_rows(void *result);
int64_t ddb_result_cols(void *result);
const uint8_t *ddb_result_col_name(void *result, int64_t col, int *out_len);
/* One of the `DDB_T_*` codes above -- translated here, never DuckDB's own enum. */
int ddb_result_col_type(void *result, int64_t col);

/* ── reading one value ──────────────────────────────────────────────────────
 *
 * One accessor per representation rather than one variant-returning call: the C
 * side then has no union to keep in step with Nova's `Value`, and a caller that
 * asks for the wrong shape gets a type error from `ddb_result_col_type` rather
 * than a reinterpreted 64 bits. */
int ddb_value_is_null(void *result, int64_t row, int64_t col);
int ddb_value_bool(void *result, int64_t row, int64_t col, int *out);
int ddb_value_i64(void *result, int64_t row, int64_t col, int64_t *out);
int ddb_value_u64(void *result, int64_t row, int64_t col, uint64_t *out);
int ddb_value_f64(void *result, int64_t row, int64_t col, double *out);
/* 128-bit as two halves; see the header comment on why not __int128. */
int ddb_value_hugeint(void *result, int64_t row, int64_t col, int64_t *out_hi, uint64_t *out_lo);
int ddb_value_decimal(void *result, int64_t row, int64_t col,
                      int64_t *out_hi, uint64_t *out_lo, int *out_scale);
/* Microseconds since the Unix epoch, UTC. */
int ddb_value_timestamp(void *result, int64_t row, int64_t col, int64_t *out_us);
/* Days since the Unix epoch. */
int ddb_value_date(void *result, int64_t row, int64_t col, int32_t *out_days);
/* Microseconds since midnight. */
int ddb_value_time(void *result, int64_t row, int64_t col, int64_t *out_us);
/* Months, days, microseconds. A non-zero month count is `DDB_E_TYPE`: Nova's
 * `Duration` has no months, and silently dropping them would move a date. */
int ddb_value_interval(void *result, int64_t row, int64_t col,
                       int32_t *out_months, int32_t *out_days, int64_t *out_us);
/* UUID as sixteen bytes, most significant first. `out` must have room for 16. */
int ddb_value_uuid(void *result, int64_t row, int64_t col, uint8_t *out16);
/* Text and blobs: the pointer is owned by the result and dies with it. */
const uint8_t *ddb_value_bytes(void *result, int64_t row, int64_t col, int *out_len);

/* ── prepared statements ────────────────────────────────────────────────────*/

void *ddb_prepare(void *conn, const uint8_t *sql, int sql_len, int *out_err);
void ddb_stmt_free(void *stmt);
int64_t ddb_stmt_param_count(void *stmt);
void *ddb_stmt_execute(void *stmt, int *out_err);
int ddb_stmt_clear(void *stmt);

/* Parameters are one-based, as DuckDB numbers them. */
int ddb_bind_null(void *stmt, int64_t i);
int ddb_bind_bool(void *stmt, int64_t i, int v);
int ddb_bind_i64(void *stmt, int64_t i, int64_t v);
int ddb_bind_u64(void *stmt, int64_t i, uint64_t v);
int ddb_bind_f64(void *stmt, int64_t i, double v);
int ddb_bind_hugeint(void *stmt, int64_t i, int64_t hi, uint64_t lo);
int ddb_bind_decimal(void *stmt, int64_t i, int64_t hi, uint64_t lo, int scale);
int ddb_bind_timestamp(void *stmt, int64_t i, int64_t us);
int ddb_bind_date(void *stmt, int64_t i, int32_t days);
int ddb_bind_time(void *stmt, int64_t i, int64_t us);
int ddb_bind_uuid(void *stmt, int64_t i, const uint8_t *b16);
int ddb_bind_bytes(void *stmt, int64_t i, const uint8_t *p, int len, int is_blob);

/* ── appender ───────────────────────────────────────────────────────────────
 *
 * The only fast way in: a row-at-a-time `INSERT` costs about two milliseconds in
 * DuckDB (spike), a hundred thousand appended rows cost about ten. */

void *ddb_appender_create(void *conn, const uint8_t *schema, int schema_len,
                          const uint8_t *table, int table_len, int *out_err);
int ddb_append_null(void *app);
int ddb_append_bool(void *app, int v);
int ddb_append_i64(void *app, int64_t v);
int ddb_append_u64(void *app, uint64_t v);
int ddb_append_f64(void *app, double v);
int ddb_append_hugeint(void *app, int64_t hi, uint64_t lo);
int ddb_append_timestamp(void *app, int64_t us);
int ddb_append_date(void *app, int32_t days);
int ddb_append_time(void *app, int64_t us);
int ddb_append_uuid(void *app, const uint8_t *b16);
int ddb_append_bytes(void *app, const uint8_t *p, int len, int is_blob);
int ddb_append_end_row(void *app);
int ddb_appender_flush(void *app);
int ddb_appender_close(void *app);

#ifdef __cplusplus
}
#endif

#endif /* NOVA_DUCKDB_SHIM_H */
