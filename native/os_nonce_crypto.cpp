// os_nonce_crypto.cpp -- DuckDB's AES-GCM, with nonces from the operating system.
//
// WHY THIS FILE EXISTS. DuckDB 1.5.5 refuses to WRITE an encrypted database with the
// mbedTLS it carries: "read-only crypto module loaded ... LOAD httpfs ... or SET
// force_mbedtls_unsafe". The cipher is sound; the random source is not -- its
// `GenerateRandomData` is a `RandomEngine` (PCG), and a nonce repeated under one key
// breaks GCM. httpfs would bring OpenSSL and a network file system into a process
// that must not touch the network; `force_mbedtls_unsafe` is what it says.
//
// So the AES-GCM PRIMITIVE is DuckDB's own (`AESStateMBEDTLS`, nothing re-implemented
// here) and only the nonce source changes: `BCryptGenRandom` on Windows, `getrandom`
// on Linux. The state is installed as the database's `DBConfig::encryption_util`,
// which DuckDB consults before it would refuse (src/main/database.cpp,
// `GetEncryptionUtil`).
//
// PRICE, NAMED: this reaches into DuckDB's internals -- `DatabaseWrapper` from the C
// API, `DBConfig::encryption_util`, and the `AESStateMBEDTLS` class -- and must be
// checked against every DuckDB upgrade. The README of the package lists exactly what
// to check.
//
// C++ because the classes are C++. It is compiled as a shim beside `duckdb_shim.c`
// (the compiler picks the language from the extension); the C shim calls the one
// `extern "C"` function below.

#include "duckdb.h"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "mbedtls_wrapper.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <errno.h>
#include <sys/random.h>
#endif

namespace {

// Fill `out` from the OS's cryptographic random source; false if it failed.
bool os_random(unsigned char *out, size_t len) {
#ifdef _WIN32
	return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	size_t got = 0;
	while (got < len) {
		ssize_t n = getrandom(out + got, len - got, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		got += static_cast<size_t>(n);
	}
	return true;
#endif
}

// DuckDB's mbedTLS AES-GCM state; only the random source is replaced. Every nonce and
// the temporary keys DuckDB draws go through this method (encryption_functions.cpp,
// single_file_block_manager.cpp, write_ahead_log.cpp -- all call it on the state).
class OsNonceState : public duckdb_mbedtls::MbedTlsWrapper::AESStateMBEDTLS {
public:
	explicit OsNonceState(duckdb::unique_ptr<duckdb::EncryptionStateMetadata> metadata)
	    : AESStateMBEDTLS(std::move(metadata)) {
	}

	void GenerateRandomData(duckdb::data_ptr_t data, duckdb::idx_t len) override {
		if (!os_random(data, len)) {
			// Loud: a zeroed or partial nonce would be the very failure this file
			// exists to prevent.
			throw duckdb::IOException("nova-duckdb: the operating system's random source failed");
		}
	}
};

class OsNonceUtil : public duckdb::EncryptionUtil {
public:
	duckdb::shared_ptr<duckdb::EncryptionState>
	CreateEncryptionState(duckdb::unique_ptr<duckdb::EncryptionStateMetadata> metadata) const override {
		return duckdb::make_shared_ptr<OsNonceState>(std::move(metadata));
	}
};

} // namespace

// Install the OS-nonce AES-GCM on an open database. 1 on success, 0 on failure.
extern "C" int ddb_install_os_nonce_crypto(duckdb_database db) {
	if (!db) {
		return 0;
	}
	try {
		auto wrapper = reinterpret_cast<duckdb::DatabaseWrapper *>(db);
		auto &config = duckdb::DBConfig::GetConfig(*wrapper->database->instance);
		config.encryption_util = duckdb::make_shared_ptr<OsNonceUtil>();
		return 1;
	} catch (...) {
		return 0;
	}
}
