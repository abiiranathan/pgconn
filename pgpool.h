#ifndef PGPOOL_H
#define PGPOOL_H

// EntryPoint for the libpq connection pool library for postgres.
/**
 * @file pgpool.h
 * @brief Public API for a libpq-based PostgreSQL connection pool.
 *
 * The pool manages a bounded set of non-blocking connections that can be
 * acquired and released by callers. Helpers are provided to run queries,
 * parameterized queries, and prepared statements with optional timeouts.
 */

#include <libpq-fe.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque type for the pool wrapper. */
typedef struct pgpool pgpool_t;

/** Opaque type for the connection wrapper. */
typedef struct pgconn pgconn_t;

/**
 * Connection pool configuration.
 *
 * Optional fields have sensible defaults; see notes per field.
 */
typedef struct {
    // Required parameters
    const char* conninfo;  // PostgreSQL connection string

    // Optional parameters with defaults
    size_t min_connections;  // Minimum number of connections to maintain (default: 1)
    size_t max_connections;  // Maximum number of connections (default: 10)
    int connect_timeout;     // Connection timeout in seconds (default: 5)
    bool auto_reconnect;     // Automatically reconnect broken connections (default: true)

    // Callbacks
    void (*connection_init)(PGconn*);   // Called when a new connection is established
    void (*connection_close)(PGconn*);  // Called before closing a connection
} pgpool_config_t;

/**
 * Create and initialize a connection pool.
 * @param config Pool configuration (must provide `conninfo`).
 * @return Initialized pool handle, or NULL on failure.
 */
pgpool_t* pgpool_create(const pgpool_config_t* config);

/**
 * Destroy a connection pool and release all resources.
 * Safe to call with NULL.
 */
void pgpool_destroy(pgpool_t* pool);

/**
 * Acquire a connection from the pool.
 * @param pool Pool handle.
 * @param timeout_ms <0 wait forever; 0 return immediately; >0 wait up to ms.
 * @return Connection handle, or NULL on timeout/failure.
 */
pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms);

/** Release a previously acquired connection back to the pool. */
void pgpool_release(pgpool_t* pool, pgconn_t* conn);

/**
 * Execute a query without returning a `PGresult*` to the caller.
 * @return true if server returned PGRES_TUPLES_OK or PGRES_COMMAND_OK.
 */
bool pgpool_execute(pgconn_t* conn, const char* query, int timeout_ms);

/**
 * Execute a query and return the `PGresult*` to the caller.
 * Caller must `PQclear` the result.
 */
PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms);

/**
 * Execute a parameterized query without explicit prepare/deallocate.
 * Internally uses `PQsendQueryParams`. Supports zero or more parameters.
 * Caller must `PQclear` the returned result.
 */
PGresult* pgpool_query_params(pgconn_t* conn, const char* query, int n_params,
                              const Oid* param_types, const char* const* param_values,
                              const int* param_lengths, const int* param_formats, int result_format,
                              int timeout_ms);

/**
 * Prepare a named statement on the connection.
 * @param stmt_name Statement name.
 * @param query SQL with $1..$n placeholders.
 * @param n_params Number of parameters in the statement.
 * @param param_types Optional array of Oids for parameter types.
 * @return true on success, false on error.
 */
bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                    const Oid* param_types, int timeout_ms);

/**
 * Execute a previously prepared statement.
 * Caller must `PQclear` the returned result on success.
 */
PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params,
                                  const char* const* param_values, const int* param_lengths,
                                  const int* param_formats, int result_format, int timeout_ms);

/** Deallocate a previously prepared statement. */
bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms);

/** Begin a transaction on a pooled connection. */
bool pgpool_begin(pgconn_t* conn);

/** Commit a transaction on a pooled connection. */
bool pgpool_commit(pgconn_t* conn);

/** Rollback a transaction on a pooled connection. */
bool pgpool_rollback(pgconn_t* conn);

/** Get the underlying libpq `PGconn*` (use with caution). */
PGconn* pgpool_get_raw_connection(pgconn_t* conn);

/** Return the last error message for a connection. */
const char* pgpool_error_message(pgconn_t* conn);

/** Get the number of active (in-use) connections in the pool. */
size_t pgpool_active_connections(pgpool_t* pool);

/** Get the number of idle connections in the pool. */
size_t pgpool_idle_connections(pgpool_t* pool);

#ifdef __cplusplus
}
#endif

#endif  // PGPOOL_H
