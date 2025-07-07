#ifndef PGPOOL_H
#define PGPOOL_H

// EntryPoint for the libpq connection pool library for postgres.

#include <libpq-fe.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque type for the pool wrapper.
typedef struct pgpool pgpool_t;

// Opaque type for the connection wrapper.
typedef struct pgconn pgconn_t;

// Connection pool configuration
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

// Initialize a connection pool with the given configuration
pgpool_t* pgpool_create(const pgpool_config_t* config);

// Destroy a connection pool and release all resources
void pgpool_destroy(pgpool_t* pool);

// Acquire a connection from the pool
// Returns NULL if no connection is available after timeout_ms milliseconds
pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms);

// Release a connection back to the pool
void pgpool_release(pgpool_t* pool, pgconn_t* conn);

// Execute a query using a pooled connection
// Returns true on success, false on failure
bool pgpool_execute(pgconn_t* conn, const char* query, int timeout_ms);

// Query with result return
PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms);

// Prepared statement support
bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                    const Oid* param_types, int timeout_ms);

PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params,
                                  const char* const* param_values, const int* param_lengths,
                                  const int* param_formats, int result_format, int timeout_ms);

bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms);

// Begin a transaction on a pooled connection
bool pgpool_begin(pgconn_t* conn);

// Commit a transaction on a pooled connection
bool pgpool_commit(pgconn_t* conn);

// Rollback a transaction on a pooled connection
bool pgpool_rollback(pgconn_t* conn);

// Get the underlying libpq connection (use with caution)
PGconn* pgpool_get_raw_connection(pgconn_t* conn);

// Get the last error message for a connection
const char* pgpool_error_message(pgconn_t* conn);

// Get the number of active connections in the pool
size_t pgpool_active_connections(pgpool_t* pool);

// Get the number of idle connections in the pool
size_t pgpool_idle_connections(pgpool_t* pool);

#ifdef __cplusplus
}
#endif

#endif  // PGPOOL_H
