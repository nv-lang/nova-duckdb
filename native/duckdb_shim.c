/* duckdb_shim.c — the flat wrappers declared in duckdb_shim.h.
 *
 * ── WHAT IS VERIFIED AND WHAT IS NOT, read this before trusting a line ──────
 *
 * This file was written WITHOUT `duckdb.h` on the machine: there is no submodule
 * yet and the amalgamation the spike unpacked was cleaned. Two different standards
 * of evidence are mixed here, and each call site says which one it is:
 *
 *   [SPIKE] — copied from `spikes/duckdb-static/spike.c` in claude-limits, which
 *             COMPILED AND RAN against DuckDB v1.5.5 on 2026-09-06. Names,
 *             argument order and types are as that program used them.
 *
 *   [BY-NAME] — not in the spike; extrapolated from the naming scheme the spike
 *               establishes (`duckdb_value_int64` -> `duckdb_value_double`, and so
 *               on). Plausible, NOT verified. Every one of these must be checked
 *               against `duckdb.h` at the first build.
 *
 * That distinction is the point of writing it down. A reviewer can check the
 * [BY-NAME] lines in one pass; without the marks the whole file would have to be
 * re-derived. If a [BY-NAME] call turns out wrong, the fix belongs here — never a
 * workaround in Nova.
 *
 * ── ARGUMENT ORDER, the trap this file is most likely to fall into ──────────
 *
 * DuckDB's value accessors take (result, COL, ROW) — the spike's
 * `duckdb_value_int64(&res, 0, 0)` is ambiguous on its own, but the API documents
 * column first. The Nova side speaks (row, col), because that is how a person reads
 * a table. The swap happens HERE, once, in every accessor. Getting it backwards
 * would not crash — it would read the wrong cell, quietly, and only on a
 * non-square result.
 */

#include "duckdb_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duckdb.h"

/* ── strings in ──────────────────────────────────────────────────────────────
 *
 * DuckDB's C API takes NUL-terminated strings; Nova's `str` is (pointer, length)
 * and is NOT NUL-terminated. Every entry point that receives text copies it into a
 * terminated buffer first. A heap copy rather than the stack buffer the FFI
 * cookbook's sqlite example uses: a settings body or a generated `INSERT` can be
 * larger than any fixed buffer, and truncating SQL is how a query becomes a
 * different query. */

static char *dup_str(const uint8_t *p, int len) {
    if (len < 0) len = 0;
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) return NULL;
    if (len > 0) memcpy(s, p, (size_t)len);
    s[len] = '\0';
    return s;
}

/* ── handle boxes ────────────────────────────────────────────────────────────
 *
 * `duckdb_database`, `duckdb_connection` and friends are themselves opaque
 * pointers, but their destroy functions take a POINTER TO the handle
 * (`duckdb_close(&db)`) [SPIKE]. Nova holds one `void*` per object, so each is
 * boxed: the box holds the handle and the box's address is what crosses. This also
 * gives every handle somewhere to keep its last error message, which the C API
 * otherwise attaches to a result rather than to a connection. */

typedef struct { duckdb_database db; char *err; } DdbDatabase;
typedef struct { duckdb_connection con; char *err; } DdbConnection;
/* `strbuf` is a ONE-SLOT arena for the text of the last value read, kept apart
 * from `err` on purpose: parking a value in the error slot makes
 * `ddb_last_error` hand back the last VARCHAR as if it were a failure. One slot
 * is exactly the promise the header makes -- valid until the next call on this
 * handle. */
typedef struct { duckdb_result res; char *err; char *strbuf; unsigned char *blobbuf; int valid; } DdbResult;
typedef struct { duckdb_prepared_statement stmt; char *err; } DdbStatement;
typedef struct { duckdb_appender app; char *err; } DdbAppender;

static void set_err(char **slot, const char *msg) {
    if (*slot) { free(*slot); *slot = NULL; }
    if (!msg) return;
    size_t n = strlen(msg);
    *slot = (char *)malloc(n + 1);
    if (*slot) memcpy(*slot, msg, n + 1);
}

const uint8_t *ddb_last_error(void *handle, int kind, int *out_len) {
    const char *m = NULL;
    if (handle) {
        switch (kind) {
            case 0: m = ((DdbDatabase *)handle)->err; break;
            case 1: m = ((DdbConnection *)handle)->err; break;
            case 2: m = ((DdbResult *)handle)->err; break;
            case 3: m = ((DdbStatement *)handle)->err; break;
            case 4: m = ((DdbAppender *)handle)->err; break;
            default: m = NULL; break;
        }
    }
    if (!m) { *out_len = 0; return (const uint8_t *)""; }
    *out_len = (int)strlen(m);
    return (const uint8_t *)m;
}

/* ── open ────────────────────────────────────────────────────────────────────*/

/* Apply one `SET`, reporting failure through the connection's error slot.
 * Settings go through SQL rather than the config API because that is what §3 of
 * the subplan specifies and what the spike proved works [SPIKE]. */
static int apply_setting(DdbConnection *c, const char *sql) {
    duckdb_result r;
    if (duckdb_query(c->con, sql, &r) != DuckDBSuccess) {   /* [SPIKE] */
        set_err(&c->err, duckdb_result_error(&r));          /* [SPIKE] */
        duckdb_destroy_result(&r);                          /* [SPIKE] */
        return 0;
    }
    duckdb_destroy_result(&r);
    return 1;
}

void *ddb_open(const uint8_t *path, int path_len,
               const uint8_t *temp_dir, int temp_dir_len,
               const uint8_t *memory_limit, int memory_limit_len,
               int threads, int read_only,
               const uint8_t *encryption_key, int encryption_key_len,
               int *out_err) {
    *out_err = DDBC_OK;

    /* ENCRYPTION IS REFUSED RATHER THAN GUESSED. 01.2 §12 wants the file
     * encrypted, and DuckDB does support it, but the C-API spelling is not in the
     * spike and inventing it here would produce a call that compiles and does
     * nothing — the worst possible outcome for a security setting. It arrives when
     * `duckdb.h` is on the machine to read. */
    if (encryption_key_len > 0) {
        *out_err = DDBC_E_OPEN;
        return NULL;
    }

    DdbDatabase *d = (DdbDatabase *)calloc(1, sizeof(DdbDatabase));
    if (!d) { *out_err = DDBC_E_OPEN; return NULL; }

    char *p = dup_str(path, path_len);
    if (!p) { free(d); *out_err = DDBC_E_OPEN; return NULL; }
    int rc = duckdb_open(p, &d->db);                        /* [SPIKE] */
    free(p);
    if (rc != DuckDBSuccess) {
        set_err(&d->err, "duckdb_open failed");
        *out_err = DDBC_E_OPEN;
        /* The box survives so the caller can read the message, then closes it. */
        return d;
    }

    /* The settings of §3 are applied on a throwaway connection: they are database
     * scoped, and doing it here is what makes them unreachable from Nova. */
    DdbConnection tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (duckdb_connect(d->db, &tmp.con) != DuckDBSuccess) { /* [SPIKE] */
        set_err(&d->err, "duckdb_connect failed while applying settings");
        *out_err = DDBC_E_OPEN;
        return d;
    }

    int ok = 1;
    /* Autoloading OFF first and unconditionally: with it on, the first call to a
     * core_functions function goes to the network and hangs ~22 s (spike). */
    ok = ok && apply_setting(&tmp, "SET autoinstall_known_extensions=false");
    ok = ok && apply_setting(&tmp, "SET autoload_known_extensions=false");
    ok = ok && apply_setting(&tmp, "SET TimeZone='UTC'");

    if (ok && temp_dir_len > 0) {
        char *t = dup_str(temp_dir, temp_dir_len);
        if (t) {
            size_t n = strlen(t) + 32;
            char *stmt = (char *)malloc(n);
            if (stmt) {
                snprintf(stmt, n, "SET temp_directory='%s'", t);
                ok = apply_setting(&tmp, stmt);
                free(stmt);
            }
            free(t);
        }
    }
    if (ok && memory_limit_len > 0) {
        char *m = dup_str(memory_limit, memory_limit_len);
        if (m) {
            size_t n = strlen(m) + 32;
            char *stmt = (char *)malloc(n);
            if (stmt) {
                snprintf(stmt, n, "SET memory_limit='%s'", m);
                ok = apply_setting(&tmp, stmt);
                free(stmt);
            }
            free(m);
        }
    }
    if (ok && threads > 0) {
        char stmt[64];
        snprintf(stmt, sizeof(stmt), "SET threads=%d", threads);
        ok = apply_setting(&tmp, stmt);
    }
    if (ok && read_only) {
        /* Read-only is a database-open flag in DuckDB, not a SET. Refused rather
         * than silently ignored: a caller asking for read-only and getting a
         * writable handle is worse than an error. [BY-NAME gap — needs
         * `duckdb_open_ext` with a config, which is not in the spike.] */
        set_err(&d->err, "read_only is not implemented in 0.1: it needs duckdb_open_ext with a config");
        ok = 0;
        *out_err = DDBC_E_OPEN;
    }

    if (!ok && *out_err == DDBC_OK) {
        set_err(&d->err, tmp.err ? tmp.err : "applying the mandatory settings failed");
        *out_err = DDBC_E_OPEN;
    }
    if (tmp.err) free(tmp.err);
    duckdb_disconnect(&tmp.con);                            /* [SPIKE] */
    return d;
}

void *ddb_connect(void *db, int *out_err) {
    *out_err = DDBC_OK;
    DdbDatabase *d = (DdbDatabase *)db;
    if (!d) { *out_err = DDBC_E_CLOSED; return NULL; }
    DdbConnection *c = (DdbConnection *)calloc(1, sizeof(DdbConnection));
    if (!c) { *out_err = DDBC_E_OPEN; return NULL; }
    if (duckdb_connect(d->db, &c->con) != DuckDBSuccess) {  /* [SPIKE] */
        set_err(&d->err, "duckdb_connect failed");
        *out_err = DDBC_E_OPEN;
        free(c);
        return NULL;
    }
    return c;
}

void ddb_disconnect(void *conn) {
    DdbConnection *c = (DdbConnection *)conn;
    if (!c) return;
    duckdb_disconnect(&c->con);                             /* [SPIKE] */
    if (c->err) free(c->err);
    free(c);
}

void ddb_close(void *db) {
    DdbDatabase *d = (DdbDatabase *)db;
    if (!d) return;
    duckdb_close(&d->db);                                   /* [SPIKE] */
    if (d->err) free(d->err);
    free(d);
}

const char *ddb_version(void) {
    return duckdb_library_version();                        /* [SPIKE] */
}

/* ── queries ─────────────────────────────────────────────────────────────────*/

int ddb_exec(void *conn, const uint8_t *sql, int sql_len, int64_t *out_changed) {
    DdbConnection *c = (DdbConnection *)conn;
    *out_changed = 0;
    if (!c) return DDBC_E_CLOSED;
    char *s = dup_str(sql, sql_len);
    if (!s) return DDBC_E_QUERY;
    duckdb_result r;
    int rc = duckdb_query(c->con, s, &r);                   /* [SPIKE] */
    free(s);
    if (rc != DuckDBSuccess) {
        set_err(&c->err, duckdb_result_error(&r));          /* [SPIKE] */
        duckdb_destroy_result(&r);
        return DDBC_E_QUERY;
    }
    *out_changed = (int64_t)duckdb_rows_changed(&r);        /* [BY-NAME] */
    duckdb_destroy_result(&r);
    return DDBC_OK;
}

void *ddb_query(void *conn, const uint8_t *sql, int sql_len, int *out_err) {
    *out_err = DDBC_OK;
    DdbConnection *c = (DdbConnection *)conn;
    if (!c) { *out_err = DDBC_E_CLOSED; return NULL; }
    char *s = dup_str(sql, sql_len);
    if (!s) { *out_err = DDBC_E_QUERY; return NULL; }
    DdbResult *out = (DdbResult *)calloc(1, sizeof(DdbResult));
    if (!out) { free(s); *out_err = DDBC_E_QUERY; return NULL; }
    int rc = duckdb_query(c->con, s, &out->res);            /* [SPIKE] */
    free(s);
    if (rc != DuckDBSuccess) {
        set_err(&c->err, duckdb_result_error(&out->res));   /* [SPIKE] */
        duckdb_destroy_result(&out->res);
        free(out);
        *out_err = DDBC_E_QUERY;
        return NULL;
    }
    out->valid = 1;
    return out;
}

void ddb_result_free(void *result) {
    DdbResult *r = (DdbResult *)result;
    if (!r) return;
    if (r->valid) duckdb_destroy_result(&r->res);           /* [SPIKE] */
    if (r->err) free(r->err);
    if (r->strbuf) duckdb_free(r->strbuf);                  /* [BY-NAME] */
    if (r->blobbuf) duckdb_free(r->blobbuf);                /* [BY-NAME] */
    free(r);
}

int64_t ddb_result_rows(void *result) {
    DdbResult *r = (DdbResult *)result;
    if (!r || !r->valid) return 0;
    return (int64_t)duckdb_row_count(&r->res);              /* [SPIKE] */
}

int64_t ddb_result_cols(void *result) {
    DdbResult *r = (DdbResult *)result;
    if (!r || !r->valid) return 0;
    return (int64_t)duckdb_column_count(&r->res);           /* [BY-NAME] */
}

const uint8_t *ddb_result_col_name(void *result, int64_t col, int *out_len) {
    DdbResult *r = (DdbResult *)result;
    *out_len = 0;
    if (!r || !r->valid) return (const uint8_t *)"";
    const char *n = duckdb_column_name(&r->res, (idx_t)col); /* [BY-NAME] */
    if (!n) return (const uint8_t *)"";
    *out_len = (int)strlen(n);
    return (const uint8_t *)n;
}

int ddb_result_col_type(void *result, int64_t col) {
    DdbResult *r = (DdbResult *)result;
    if (!r || !r->valid) return DDBC_T_UNSUPPORTED;
    /* [BY-NAME] `duckdb_column_type` and the `DUCKDB_TYPE_*` constants.
     *
     * The switch is exhaustive over what 0.1 reads and falls through to
     * UNSUPPORTED for everything else. That default is deliberate: a DuckDB
     * version adding a type must not make an existing column read as something
     * else, and `UNSUPPORTED` becomes a named refusal on the Nova side. */
    switch (duckdb_column_type(&r->res, (idx_t)col)) {
        case DUCKDB_TYPE_BOOLEAN:      return DDBC_T_BOOLEAN;
        case DUCKDB_TYPE_TINYINT:      return DDBC_T_TINYINT;
        case DUCKDB_TYPE_SMALLINT:     return DDBC_T_SMALLINT;
        case DUCKDB_TYPE_INTEGER:      return DDBC_T_INTEGER;
        case DUCKDB_TYPE_BIGINT:       return DDBC_T_BIGINT;
        case DUCKDB_TYPE_UTINYINT:     return DDBC_T_UTINYINT;
        case DUCKDB_TYPE_USMALLINT:    return DDBC_T_USMALLINT;
        case DUCKDB_TYPE_UINTEGER:     return DDBC_T_UINTEGER;
        case DUCKDB_TYPE_UBIGINT:      return DDBC_T_UBIGINT;
        case DUCKDB_TYPE_HUGEINT:      return DDBC_T_HUGEINT;
        case DUCKDB_TYPE_FLOAT:        return DDBC_T_FLOAT;
        case DUCKDB_TYPE_DOUBLE:       return DDBC_T_DOUBLE;
        case DUCKDB_TYPE_DECIMAL:      return DDBC_T_DECIMAL;
        case DUCKDB_TYPE_VARCHAR:      return DDBC_T_VARCHAR;
        case DUCKDB_TYPE_BLOB:         return DDBC_T_BLOB;
        case DUCKDB_TYPE_TIMESTAMP:    return DDBC_T_TIMESTAMP;
        case DUCKDB_TYPE_TIMESTAMP_TZ: return DDBC_T_TIMESTAMPTZ;
        case DUCKDB_TYPE_DATE:         return DDBC_T_DATE;
        case DUCKDB_TYPE_TIME:         return DDBC_T_TIME;
        case DUCKDB_TYPE_INTERVAL:     return DDBC_T_INTERVAL;
        case DUCKDB_TYPE_ENUM:         return DDBC_T_ENUM;
        case DUCKDB_TYPE_UUID:         return DDBC_T_UUID;
        case DUCKDB_TYPE_SQLNULL:      return DDBC_T_SQLNULL;
        default:                       return DDBC_T_UNSUPPORTED;
    }
}

/* ── reading one value ───────────────────────────────────────────────────────
 *
 * Note the (col, row) order handed to DuckDB against the (row, col) taken from
 * Nova — see the header comment of this file. */

int ddb_value_is_null(void *result, int64_t row, int64_t col) {
    DdbResult *r = (DdbResult *)result;
    if (!r || !r->valid) return 1;
    return duckdb_value_is_null(&r->res, (idx_t)col, (idx_t)row) ? 1 : 0; /* [BY-NAME] */
}

#define DDB_GUARD(r) DdbResult *r = (DdbResult *)result; if (!r || !r->valid) return DDBC_E_CLOSED;

int ddb_value_bool(void *result, int64_t row, int64_t col, int *out) {
    DDB_GUARD(r)
    *out = duckdb_value_boolean(&r->res, (idx_t)col, (idx_t)row) ? 1 : 0; /* [BY-NAME] */
    return DDBC_OK;
}

int ddb_value_i64(void *result, int64_t row, int64_t col, int64_t *out) {
    DDB_GUARD(r)
    *out = (int64_t)duckdb_value_int64(&r->res, (idx_t)col, (idx_t)row);  /* [SPIKE] */
    return DDBC_OK;
}

int ddb_value_u64(void *result, int64_t row, int64_t col, uint64_t *out) {
    DDB_GUARD(r)
    *out = (uint64_t)duckdb_value_uint64(&r->res, (idx_t)col, (idx_t)row); /* [BY-NAME] */
    return DDBC_OK;
}

int ddb_value_f64(void *result, int64_t row, int64_t col, double *out) {
    DDB_GUARD(r)
    *out = duckdb_value_double(&r->res, (idx_t)col, (idx_t)row);          /* [BY-NAME] */
    return DDBC_OK;
}

int ddb_value_hugeint(void *result, int64_t row, int64_t col, int64_t *out_hi, uint64_t *out_lo) {
    DDB_GUARD(r)
    duckdb_hugeint h = duckdb_value_hugeint(&r->res, (idx_t)col, (idx_t)row); /* [BY-NAME] */
    *out_hi = h.upper;
    *out_lo = h.lower;
    return DDBC_OK;
}

int ddb_value_decimal(void *result, int64_t row, int64_t col,
                      int64_t *out_hi, uint64_t *out_lo, int *out_scale) {
    DDB_GUARD(r)
    duckdb_decimal d = duckdb_value_decimal(&r->res, (idx_t)col, (idx_t)row); /* [BY-NAME] */
    *out_hi = d.value.upper;
    *out_lo = d.value.lower;
    *out_scale = (int)d.scale;
    return DDBC_OK;
}

int ddb_value_timestamp(void *result, int64_t row, int64_t col, int64_t *out_us) {
    DDB_GUARD(r)
    duckdb_timestamp t = duckdb_value_timestamp(&r->res, (idx_t)col, (idx_t)row); /* [BY-NAME]; the struct's `.micros` is [SPIKE] */
    *out_us = t.micros;
    return DDBC_OK;
}

int ddb_value_date(void *result, int64_t row, int64_t col, int32_t *out_days) {
    DDB_GUARD(r)
    duckdb_date d = duckdb_value_date(&r->res, (idx_t)col, (idx_t)row);   /* [BY-NAME] */
    *out_days = d.days;
    return DDBC_OK;
}

int ddb_value_time(void *result, int64_t row, int64_t col, int64_t *out_us) {
    DDB_GUARD(r)
    duckdb_time t = duckdb_value_time(&r->res, (idx_t)col, (idx_t)row);   /* [BY-NAME] */
    *out_us = t.micros;
    return DDBC_OK;
}

int ddb_value_interval(void *result, int64_t row, int64_t col,
                       int32_t *out_months, int32_t *out_days, int64_t *out_us) {
    DDB_GUARD(r)
    duckdb_interval v = duckdb_value_interval(&r->res, (idx_t)col, (idx_t)row); /* [BY-NAME] */
    *out_months = v.months;
    *out_days = v.days;
    *out_us = v.micros;
    return DDBC_OK;
}

int ddb_value_uuid(void *result, int64_t row, int64_t col, uint8_t *out16) {
    DDB_GUARD(r)
    /* [BY-NAME] DuckDB stores a UUID as a HUGEINT with the sign bit flipped so that
     * ordering matches the textual form. Undoing that flip here rather than in Nova
     * keeps the quirk in the file that talks to DuckDB. */
    duckdb_hugeint h = duckdb_value_hugeint(&r->res, (idx_t)col, (idx_t)row);
    uint64_t hi = (uint64_t)h.upper ^ 0x8000000000000000ULL;
    uint64_t lo = h.lower;
    for (int i = 0; i < 8; i++) out16[i] = (uint8_t)(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) out16[8 + i] = (uint8_t)(lo >> (56 - 8 * i));
    return DDBC_OK;
}

const uint8_t *ddb_value_bytes(void *result, int64_t row, int64_t col, int *out_len) {
    DdbResult *r = (DdbResult *)result;
    *out_len = 0;
    if (!r || !r->valid) return (const uint8_t *)"";
    /* [BY-NAME] `duckdb_value_varchar` allocates with DuckDB's allocator and the
     * caller frees with `duckdb_free`. The previous value is released first, which
     * is what makes the header's promise true and no more than true: the pointer
     * is valid until the NEXT call on this handle. */
    if (r->strbuf) { duckdb_free(r->strbuf); r->strbuf = NULL; }
    char *v = duckdb_value_varchar(&r->res, (idx_t)col, (idx_t)row);
    if (!v) return (const uint8_t *)"";
    r->strbuf = v;
    *out_len = (int)strlen(v);
    return (const uint8_t *)v;
}

const uint8_t *ddb_value_blob(void *result, int64_t row, int64_t col, int *out_len) {
    DdbResult *r = (DdbResult *)result;
    *out_len = 0;
    if (!r || !r->valid) return (const uint8_t *)"";
    /* A SEPARATE ACCESSOR, and not a convenience. `duckdb_value_varchar` CONVERTS
     * the value to text; handing a BLOB through it returns a rendering, not the
     * bytes. [BY-NAME] `duckdb_blob` carries `.data` and `.size`, and `.data` is
     * freed with `duckdb_free`. */
    if (r->blobbuf) { duckdb_free(r->blobbuf); r->blobbuf = NULL; }
    duckdb_blob b = duckdb_value_blob(&r->res, (idx_t)col, (idx_t)row);
    if (!b.data || b.size == 0) return (const uint8_t *)"";
    r->blobbuf = (unsigned char *)b.data;
    *out_len = (int)b.size;
    return (const uint8_t *)b.data;
}

/* ── prepared statements ─────────────────────────────────────────────────────
 *
 * Every one of these was declared in the header and bound from Nova before a single
 * line of it existed here (found 2026-09-08, the first evening the link got far
 * enough to ask). The statement box carries its own error slot, like every other
 * handle, so a bind failure has somewhere to say why. */

void *ddb_prepare(void *conn, const uint8_t *sql, int sql_len, int *out_err) {
    *out_err = DDBC_OK;
    DdbConnection *c = (DdbConnection *)conn;
    if (!c) { *out_err = DDBC_E_CLOSED; return NULL; }
    char *s = dup_str(sql, sql_len);
    if (!s) { *out_err = DDBC_E_PREPARE; return NULL; }
    DdbStatement *st = (DdbStatement *)calloc(1, sizeof(DdbStatement));
    if (!st) { free(s); *out_err = DDBC_E_PREPARE; return NULL; }
    int rc = duckdb_prepare(c->con, s, &st->stmt);                  /* [BY-NAME] */
    free(s);
    if (rc != DuckDBSuccess) {
        /* The message lives on the STATEMENT even on failure, and it is the only
         * place it lives: copy it out before destroying the handle. */
        set_err(&c->err, duckdb_prepare_error(st->stmt));           /* [BY-NAME] */
        duckdb_destroy_prepare(&st->stmt);                          /* [BY-NAME] */
        free(st);
        *out_err = DDBC_E_PREPARE;
        return NULL;
    }
    return st;
}

void ddb_stmt_free(void *stmt) {
    DdbStatement *st = (DdbStatement *)stmt;
    if (!st) return;
    duckdb_destroy_prepare(&st->stmt);                              /* [BY-NAME] */
    if (st->err) free(st->err);
    free(st);
}

int64_t ddb_stmt_param_count(void *stmt) {
    DdbStatement *st = (DdbStatement *)stmt;
    if (!st) return 0;
    return (int64_t)duckdb_nparams(st->stmt);                       /* [BY-NAME] */
}

void *ddb_stmt_execute(void *stmt, int *out_err) {
    *out_err = DDBC_OK;
    DdbStatement *st = (DdbStatement *)stmt;
    if (!st) { *out_err = DDBC_E_CLOSED; return NULL; }
    DdbResult *out = (DdbResult *)calloc(1, sizeof(DdbResult));
    if (!out) { *out_err = DDBC_E_QUERY; return NULL; }
    if (duckdb_execute_prepared(st->stmt, &out->res) != DuckDBSuccess) { /* [BY-NAME] */
        set_err(&st->err, duckdb_result_error(&out->res));          /* [SPIKE] */
        duckdb_destroy_result(&out->res);
        free(out);
        *out_err = DDBC_E_QUERY;
        return NULL;
    }
    out->valid = 1;
    return out;
}

int ddb_stmt_clear(void *stmt) {
    DdbStatement *st = (DdbStatement *)stmt;
    if (!st) return DDBC_E_CLOSED;
    return duckdb_clear_bindings(st->stmt) == DuckDBSuccess         /* [BY-NAME] */
        ? DDBC_OK : DDBC_E_BIND;
}

/* Positions are 1-based on BOTH sides -- Nova's `?1` numbering and DuckDB's C API --
 * so the index crosses unchanged. Stated once here rather than in twelve places. */
#define DDB_BIND(call) do { \
    DdbStatement *st = (DdbStatement *)stmt; \
    if (!st) return DDBC_E_CLOSED; \
    return (call) == DuckDBSuccess ? DDBC_OK : DDBC_E_BIND; \
} while (0)

int ddb_bind_null(void *stmt, int64_t i) {
    DDB_BIND(duckdb_bind_null(st->stmt, (idx_t)i));                 /* [BY-NAME] */
}
int ddb_bind_bool(void *stmt, int64_t i, int v) {
    DDB_BIND(duckdb_bind_boolean(st->stmt, (idx_t)i, v != 0));      /* [BY-NAME] */
}
int ddb_bind_i64(void *stmt, int64_t i, int64_t v) {
    DDB_BIND(duckdb_bind_int64(st->stmt, (idx_t)i, v));             /* [BY-NAME] */
}
int ddb_bind_u64(void *stmt, int64_t i, uint64_t v) {
    DDB_BIND(duckdb_bind_uint64(st->stmt, (idx_t)i, v));            /* [BY-NAME] */
}
int ddb_bind_f64(void *stmt, int64_t i, double v) {
    DDB_BIND(duckdb_bind_double(st->stmt, (idx_t)i, v));            /* [BY-NAME] */
}
int ddb_bind_hugeint(void *stmt, int64_t i, int64_t hi, uint64_t lo) {
    duckdb_hugeint h; h.upper = hi; h.lower = lo;                   /* [SPIKE] */
    DDB_BIND(duckdb_bind_hugeint(st->stmt, (idx_t)i, h));           /* [BY-NAME] */
}
int ddb_bind_decimal(void *stmt, int64_t i, int64_t hi, uint64_t lo, int scale) {
    /* `width` is not a parameter of the Nova side on purpose: the column already
     * has one, and a value cannot widen it. 38 is DECIMAL's maximum, so it never
     * narrows what the column allows either. */
    duckdb_decimal d;
    d.width = 38;
    d.scale = (uint8_t)scale;
    d.value.upper = hi;
    d.value.lower = lo;                                             /* [SPIKE] */
    DDB_BIND(duckdb_bind_decimal(st->stmt, (idx_t)i, d));           /* [BY-NAME] */
}
int ddb_bind_timestamp(void *stmt, int64_t i, int64_t us) {
    duckdb_timestamp t; t.micros = us;                              /* [SPIKE] */
    DDB_BIND(duckdb_bind_timestamp(st->stmt, (idx_t)i, t));         /* [BY-NAME] */
}
int ddb_bind_date(void *stmt, int64_t i, int32_t days) {
    duckdb_date d; d.days = days;                                   /* [SPIKE] */
    DDB_BIND(duckdb_bind_date(st->stmt, (idx_t)i, d));              /* [BY-NAME] */
}
int ddb_bind_time(void *stmt, int64_t i, int64_t us) {
    duckdb_time t; t.micros = us;                                   /* [SPIKE] */
    DDB_BIND(duckdb_bind_time(st->stmt, (idx_t)i, t));              /* [BY-NAME] */
}
int ddb_bind_uuid(void *stmt, int64_t i, const uint8_t *b16) {
    /* A UUID crosses as sixteen bytes, big-endian as the text form reads, and DuckDB
     * stores it as a hugeint whose HIGH bit is flipped -- that flip is how it orders
     * UUIDs the way a person expects. Both halves are assembled here so no caller
     * has to know either fact. */
    if (!b16) return DDBC_E_TYPE;
    uint64_t hi = 0, lo = 0;
    for (int k = 0; k < 8; k++)  hi = (hi << 8) | b16[k];
    for (int k = 8; k < 16; k++) lo = (lo << 8) | b16[k];
    duckdb_hugeint h;
    h.upper = (int64_t)(hi ^ 0x8000000000000000ULL);
    h.lower = lo;
    DDB_BIND(duckdb_bind_hugeint(st->stmt, (idx_t)i, h));           /* [BY-NAME] */
}
int ddb_bind_bytes(void *stmt, int64_t i, const uint8_t *p, int len, int is_blob) {
    DdbStatement *st = (DdbStatement *)stmt;
    if (!st) return DDBC_E_CLOSED;
    if (len < 0) len = 0;
    if (is_blob) {
        /* A BLOB takes pointer+length and needs no terminator. */
        return duckdb_bind_blob(st->stmt, (idx_t)i, p, (idx_t)len)  /* [BY-NAME] */
            == DuckDBSuccess ? DDBC_OK : DDBC_E_BIND;
    }
    /* VARCHAR: `duckdb_bind_varchar_length` also takes a length, so no copy and no
     * terminator is needed -- unlike the SQL entry points above, which do. */
    return duckdb_bind_varchar_length(st->stmt, (idx_t)i,           /* [BY-NAME] */
                                      (const char *)p, (idx_t)len)
        == DuckDBSuccess ? DDBC_OK : DDBC_E_BIND;
}

#undef DDB_BIND

/* ── appender ────────────────────────────────────────────────────────────────
 *
 * The appender has no positions: it advances a cursor, one value per call, and
 * `ddb_append_end_row` closes the row. That is why none of these takes an index. */

void *ddb_appender_create(void *conn, const uint8_t *schema, int schema_len,
                          const uint8_t *table, int table_len, int *out_err) {
    *out_err = DDBC_OK;
    DdbConnection *c = (DdbConnection *)conn;
    if (!c) { *out_err = DDBC_E_CLOSED; return NULL; }
    /* An empty schema means "the default one", and DuckDB spells that NULL rather
     * than "": passing the empty string looks for a schema of that name. */
    char *sch = schema_len > 0 ? dup_str(schema, schema_len) : NULL;
    char *tab = dup_str(table, table_len);
    if (!tab) { if (sch) free(sch); *out_err = DDBC_E_APPEND; return NULL; }
    DdbAppender *a = (DdbAppender *)calloc(1, sizeof(DdbAppender));
    if (!a) { if (sch) free(sch); free(tab); *out_err = DDBC_E_APPEND; return NULL; }
    int rc = duckdb_appender_create(c->con, sch, tab, &a->app);     /* [SPIKE] */
    if (sch) free(sch);
    free(tab);
    if (rc != DuckDBSuccess) {
        set_err(&c->err, duckdb_appender_error(a->app));            /* [BY-NAME] */
        duckdb_appender_destroy(&a->app);                           /* [BY-NAME] */
        free(a);
        *out_err = DDBC_E_APPEND;
        return NULL;
    }
    return a;
}

#define DDB_APPEND(call) do { \
    DdbAppender *a = (DdbAppender *)app; \
    if (!a) return DDBC_E_CLOSED; \
    if ((call) == DuckDBSuccess) return DDBC_OK; \
    set_err(&a->err, duckdb_appender_error(a->app)); \
    return DDBC_E_APPEND; \
} while (0)

int ddb_append_null(void *app) {
    DDB_APPEND(duckdb_append_null(a->app));                         /* [BY-NAME] */
}
int ddb_append_bool(void *app, int v) {
    DDB_APPEND(duckdb_append_bool(a->app, v != 0));                 /* [BY-NAME] */
}
int ddb_append_i64(void *app, int64_t v) {
    DDB_APPEND(duckdb_append_int64(a->app, v));                     /* [SPIKE] */
}
int ddb_append_u64(void *app, uint64_t v) {
    DDB_APPEND(duckdb_append_uint64(a->app, v));                    /* [BY-NAME] */
}
int ddb_append_f64(void *app, double v) {
    DDB_APPEND(duckdb_append_double(a->app, v));                    /* [BY-NAME] */
}
int ddb_append_hugeint(void *app, int64_t hi, uint64_t lo) {
    duckdb_hugeint h; h.upper = hi; h.lower = lo;                   /* [SPIKE] */
    DDB_APPEND(duckdb_append_hugeint(a->app, h));                   /* [BY-NAME] */
}
int ddb_append_timestamp(void *app, int64_t us) {
    duckdb_timestamp t; t.micros = us;                              /* [SPIKE] */
    DDB_APPEND(duckdb_append_timestamp(a->app, t));                 /* [BY-NAME] */
}
int ddb_append_date(void *app, int32_t days) {
    duckdb_date d; d.days = days;                                   /* [SPIKE] */
    DDB_APPEND(duckdb_append_date(a->app, d));                      /* [BY-NAME] */
}
int ddb_append_time(void *app, int64_t us) {
    duckdb_time t; t.micros = us;                                   /* [SPIKE] */
    DDB_APPEND(duckdb_append_time(a->app, t));                      /* [BY-NAME] */
}
int ddb_append_uuid(void *app, const uint8_t *b16) {
    /* Same assembly and the same high-bit flip as ddb_bind_uuid; the two must agree
     * or a row written through the appender sorts differently from one written
     * through a statement. */
    if (!b16) return DDBC_E_TYPE;
    uint64_t hi = 0, lo = 0;
    for (int k = 0; k < 8; k++)  hi = (hi << 8) | b16[k];
    for (int k = 8; k < 16; k++) lo = (lo << 8) | b16[k];
    duckdb_hugeint h;
    h.upper = (int64_t)(hi ^ 0x8000000000000000ULL);
    h.lower = lo;
    DDB_APPEND(duckdb_append_hugeint(a->app, h));                   /* [BY-NAME] */
}
int ddb_append_bytes(void *app, const uint8_t *p, int len, int is_blob) {
    DdbAppender *a = (DdbAppender *)app;
    if (!a) return DDBC_E_CLOSED;
    if (len < 0) len = 0;
    int rc = is_blob
        ? duckdb_append_blob(a->app, p, (idx_t)len)                 /* [BY-NAME] */
        : duckdb_append_varchar_length(a->app, (const char *)p, (idx_t)len);
    if (rc == DuckDBSuccess) return DDBC_OK;
    set_err(&a->err, duckdb_appender_error(a->app));                /* [BY-NAME] */
    return DDBC_E_APPEND;
}
int ddb_append_end_row(void *app) {
    DDB_APPEND(duckdb_appender_end_row(a->app));                    /* [SPIKE] */
}
int ddb_appender_flush(void *app) {
    DDB_APPEND(duckdb_appender_flush(a->app));                      /* [BY-NAME] */
}

int ddb_appender_close(void *app) {
    /* Close and destroy in one call: Nova's `Appender` is a consume type whose
     * cleanup runs exactly once, so there is no state in which a closed appender is
     * still reachable. Flushing here rather than in the caller means a dropped
     * appender cannot lose rows silently. */
    DdbAppender *a = (DdbAppender *)app;
    if (!a) return DDBC_E_CLOSED;
    int rc = duckdb_appender_close(a->app);                         /* [BY-NAME] */
    int code = rc == DuckDBSuccess ? DDBC_OK : DDBC_E_APPEND;
    duckdb_appender_destroy(&a->app);                               /* [BY-NAME] */
    if (a->err) free(a->err);
    free(a);
    return code;
}

#undef DDB_APPEND
