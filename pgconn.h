/**
 * @file pgconn.h
 * @brief A PostgreSQL connection wrapper with default non-locking and optional thread-safe APIs.
 *
 * This library provides a thin wrapper around libpq with two API variants:
 * - Default functions (pgconn_*): No internal locking. These are fast and suitable
 *   for single-threaded applications or when the caller manages synchronization.
 * - Thread-safe functions (pgconn_*_safe): Use an internal mutex for protection.
 *   They are safe for concurrent access from multiple threads. To use these,
 *   the connection must be created with `thread_safe = true` in the config.
 *
 * Design principles:
 * - Each connection wrapper (pgconn_t) contains its own mutex if created in thread-safe mode.
 * - No global state or locks.
 * - Thread-safe functions always lock -> call default function -> unlock.
 * - Deadlock avoidance: never hold multiple connection locks simultaneously.
 * - Clear error reporting with per-connection error buffers.
 */

#ifndef PGCONN_H
#define PGCONN_H

#include <libpq-fe.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct pgconn pgconn_t;

/**
 * Configuration for creating a new PostgreSQL connection.
 */
typedef struct {
    /** PostgreSQL connection string (required). */
    const char* conninfo;

    /** Connection timeout in seconds (0 = use libpq default). */
    int connect_timeout;

    /**
     * Enable thread-safe mode. If true, a mutex is initialized, allowing
     * the use of _safe API functions. Defaults to false.
     */
    bool thread_safe;

    /** Enable automatic reconnection on connection loss. */
    bool auto_reconnect;

    /** Maximum reconnection attempts (0 = infinite). */
    int max_reconnect_attempts;

    /** Optional callback invoked after successful connection. */
    void (*connection_init)(PGconn* raw_conn);

    /** Optional callback invoked before connection close. */
    void (*connection_close)(PGconn* raw_conn);
} pgconn_config_t;

/**
 * Query execution options.
 */
typedef struct {
    /** Query timeout in milliseconds (-1 = infinite, 0 = no wait). */
    int timeout_ms;

    /** Automatically retry on connection failure (uses auto_reconnect). */
    bool retry_on_failure;
} pgconn_query_opts_t;

// === Connection Management ===

/**
 * Creates a new PostgreSQL connection wrapper.
 * @param config Configuration parameters. Must not be NULL.
 * @return New connection on success, NULL on failure.
 * @note Caller must free with pgconn_destroy().
 */
pgconn_t* pgconn_create(const pgconn_config_t* config);

/**
 * Destroys a connection and frees all resources.
 * @param conn Connection to destroy. Safe to call with NULL.
 * @note Not thread-safe.
 */
void pgconn_destroy(pgconn_t* conn);

/**
 * Gets the underlying libpq connection handle.
 * @param conn Connection wrapper.
 * @return Raw PGconn pointer, or NULL if conn is NULL.
 * @note Use with caution. Direct manipulation may break wrapper state.
 */
PGconn* pgconn_get_raw(pgconn_t* conn);

/**
 * Validates that the connection is alive and responsive.
 * @param conn Connection to validate.
 * @return true if connection is healthy, false otherwise.
 * @note Not thread-safe. Caller must ensure exclusive access.
 */
bool pgconn_validate(pgconn_t* conn);

/**
 * Validates connection health (thread-safe version).
 * @param conn Connection to validate.
 * @return true if connection is healthy, false otherwise.
 */
bool pgconn_validate_safe(pgconn_t* conn);

/**
 * Attempts to reconnect a failed connection.
 * @param conn Connection to reconnect.
 * @return true on successful reconnection, false otherwise.
 * @note Not thread-safe. Caller must ensure exclusive access.
 */
bool pgconn_reconnect(pgconn_t* conn);

/**
 * Reconnects a failed connection (thread-safe version).
 * @param conn Connection to reconnect.
 * @return true on successful reconnection, false otherwise.
 */
bool pgconn_reconnect_safe(pgconn_t* conn);

// === Simple Query Execution ===

/**
 * Executes a SQL query and returns success/failure.
 * @param conn Connection to use.
 * @param query SQL query string. Must not be NULL.
 * @param opts Query execution options. NULL uses defaults.
 * @return true on success, false on failure.
 * @note Not thread-safe. For results, use pgconn_query() instead.
 */
bool pgconn_execute(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts);

/**
 * Executes a SQL query (thread-safe version).
 * @param conn Connection to use.
 * @param query SQL query string. Must not be NULL.
 * @param opts Query execution options. NULL uses defaults.
 * @return true on success, false on failure.
 */
bool pgconn_execute_safe(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts);

/**
 * Executes a SQL query and returns the result.
 * @param conn Connection to use.
 * @param query SQL query string. Must not be NULL.
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success, NULL on failure.
 * @note Not thread-safe. Caller must free result with PQclear().
 */
PGresult* pgconn_query(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts);

/**
 * Executes a SQL query and returns the result (thread-safe version).
 * @param conn Connection to use.
 * @param query SQL query string. Must not be NULL.
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success, NULL on failure.
 * @note Caller must free result with PQclear().
 */
PGresult* pgconn_query_safe(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts);

// === Parameterized Query Execution (Simplified) ===

/**
 * Executes a parameterized query using text format for all parameters.
 * @param conn Connection to use.
 * @param query SQL query with $1, $2, ... placeholders.
 * @param n_params Number of parameters.
 * @param param_values Array of parameter values (null-terminated strings).
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success (text format), NULL on failure.
 * @note Not thread-safe. Caller must free result with PQclear().
 */
PGresult* pgconn_query_params(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                              const pgconn_query_opts_t* opts);

/**
 * Executes a parameterized query using text format (thread-safe version).
 * @note See pgconn_query_params() for parameter documentation.
 */
PGresult* pgconn_query_params_safe(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                                   const pgconn_query_opts_t* opts);

// === Parameterized Query Execution (Full) ===

/**
 * Executes a parameterized query with full control over types, formats, and lengths.
 * @param conn Connection to use.
 * @param query SQL query with $1, $2, ... placeholders.
 * @param n_params Number of parameters.
 * @param param_types Array of parameter type OIDs (NULL for auto-detection).
 * @param param_values Array of parameter values as strings.
 * @param param_lengths Array of parameter lengths (NULL for null-terminated strings).
 * @param param_formats Array of parameter formats (0=text, 1=binary, NULL=all text).
 * @param result_format Result format (0=text, 1=binary).
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success, NULL on failure.
 * @note Not thread-safe. Caller must free result with PQclear().
 */
PGresult* pgconn_query_params_full(pgconn_t* conn, const char* query, int n_params, const Oid* param_types,
                                   const char* const* param_values, const int* param_lengths, const int* param_formats,
                                   int result_format, const pgconn_query_opts_t* opts);

/**
 * Executes a parameterized query with full control (thread-safe version).
 * @note See pgconn_query_params_full() for parameter documentation.
 */
PGresult* pgconn_query_params_full_safe(pgconn_t* conn, const char* query, int n_params, const Oid* param_types,
                                        const char* const* param_values, const int* param_lengths,
                                        const int* param_formats, int result_format, const pgconn_query_opts_t* opts);

// === Prepared Statements (Simplified) ===

/**
 * Executes a prepared statement using text format for all parameters.
 * @param conn Connection to use.
 * @param stmt_name Name of prepared statement. Must not be NULL.
 * @param n_params Number of parameters.
 * @param param_values Array of parameter values (null-terminated strings).
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success (text format), NULL on failure.
 * @note Not thread-safe. Caller must free result with PQclear().
 */
PGresult* pgconn_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params, const char* const* param_values,
                                  const pgconn_query_opts_t* opts);

/**
 * Executes a prepared statement using text format (thread-safe version).
 * @note See pgconn_execute_prepared() for parameter documentation.
 */
PGresult* pgconn_execute_prepared_safe(pgconn_t* conn, const char* stmt_name, int n_params,
                                       const char* const* param_values, const pgconn_query_opts_t* opts);

// === Prepared Statements (Full) ===

/**
 * Prepares a SQL statement for later execution.
 * @param conn Connection to use.
 * @param stmt_name Statement name for later reference. Must not be NULL.
 * @param query SQL query with $1, $2, ... placeholders.
 * @param n_params Number of parameters.
 * @param param_types Array of parameter type OIDs (NULL for auto-detection).
 * @return true on success, false on failure.
 * @note Not thread-safe.
 */
bool pgconn_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params, const Oid* param_types);

/**
 * Prepares a SQL statement (thread-safe version).
 * @note See pgconn_prepare() for parameter documentation.
 */
bool pgconn_prepare_safe(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                         const Oid* param_types);

/**
 * Executes a previously prepared statement with full control over formats.
 * @param conn Connection to use.
 * @param stmt_name Name of prepared statement. Must not be NULL.
 * @param n_params Number of parameters.
 * @param param_values Array of parameter values as strings.
 * @param param_lengths Array of parameter lengths (NULL for null-terminated strings).
 * @param param_formats Array of parameter formats (0=text, 1=binary, NULL=all text).
 * @param result_format Result format (0=text, 1=binary).
 * @param opts Query execution options. NULL uses defaults.
 * @return PGresult on success, NULL on failure.
 * @note Not thread-safe. Caller must free result with PQclear().
 */
PGresult* pgconn_execute_prepared_full(pgconn_t* conn, const char* stmt_name, int n_params,
                                       const char* const* param_values, const int* param_lengths,
                                       const int* param_formats, int result_format, const pgconn_query_opts_t* opts);

/**
 * Executes a prepared statement with full control (thread-safe version).
 * @note See pgconn_execute_prepared_full() for parameter documentation.
 */
PGresult* pgconn_execute_prepared_full_safe(pgconn_t* conn, const char* stmt_name, int n_params,
                                            const char* const* param_values, const int* param_lengths,
                                            const int* param_formats, int result_format,
                                            const pgconn_query_opts_t* opts);

/**
 * Deallocates a prepared statement.
 * @param conn Connection to use.
 * @param stmt_name Name of prepared statement to deallocate. Must not be NULL.
 * @return true on success, false on failure.
 * @note Not thread-safe.
 */
bool pgconn_deallocate(pgconn_t* conn, const char* stmt_name);

/**
 * Deallocates a prepared statement (thread-safe version).
 * @param conn Connection to use.
 * @param stmt_name Name of prepared statement to deallocate. Must not be NULL.
 */
bool pgconn_deallocate_safe(pgconn_t* conn, const char* stmt_name);

// === Transaction Management ===

/**
 * Begins a new transaction.
 * @param conn Connection to use.
 * @return true on success, false on failure or if transaction already active.
 * @note Not thread-safe.
 */
bool pgconn_begin(pgconn_t* conn);

/**
 * Begins a new transaction (thread-safe version).
 * @param conn Connection to use.
 * @return true on success, false on failure or if transaction already active.
 */
bool pgconn_begin_safe(pgconn_t* conn);

/**
 * Commits the current transaction.
 * @param conn Connection to use.
 * @return true on success, false on failure or if no transaction is active.
 * @note Not thread-safe.
 */
bool pgconn_commit(pgconn_t* conn);

/**
 * Commits the current transaction (thread-safe version).
 * @param conn Connection to use.
 * @return true on success, false on failure or if no transaction is active.
 */
bool pgconn_commit_safe(pgconn_t* conn);

/**
 * Rolls back the current transaction.
 * @param conn Connection to use.
 * @return true on success, false on failure or if no transaction is active.
 * @note Not thread-safe.
 */
bool pgconn_rollback(pgconn_t* conn);

/**
 * Rolls back the current transaction (thread-safe version).
 * @param conn Connection to use.
 * @return true on success, false on failure or if no transaction is active.
 */
bool pgconn_rollback_safe(pgconn_t* conn);

/**
 * Checks if a transaction is currently active.
 * @param conn Connection to check.
 * @return true if transaction is active, false otherwise.
 * @note Not thread-safe.
 */
bool pgconn_in_transaction(pgconn_t* conn);

/**
 * Checks if a transaction is currently active (thread-safe version).
 * @param conn Connection to check.
 * @return true if transaction is active, false otherwise.
 */
bool pgconn_in_transaction_safe(pgconn_t* conn);

// === Error Handling ===

/**
 * Gets the last error message for this connection.
 * @param conn Connection to query.
 * @return Error message string, never NULL.
 * @note Not thread-safe. Returns pointer to internal buffer.
 */
const char* pgconn_error_message(pgconn_t* conn);

/**
 * Gets the last error message (thread-safe version).
 * @param conn Connection to query.
 * @return Error message string, never NULL.
 * @note Returns pointer to internal buffer, protected by a lock.
 */
const char* pgconn_error_message_safe(pgconn_t* conn);

/**
 * Clears the last error message.
 * @param conn Connection to clear error for.
 * @note Not thread-safe.
 */
void pgconn_clear_error(pgconn_t* conn);

/**
 * Clears the last error message (thread-safe version).
 * @param conn Connection to clear error for.
 */
void pgconn_clear_error_safe(pgconn_t* conn);

// === Connection State ===

/**
 * Gets the current connection status.
 * @param conn Connection to check.
 * @return ConnStatusType from libpq.
 * @note Not thread-safe.
 */
ConnStatusType pgconn_status(pgconn_t* conn);

/**
 * Gets the current connection status (thread-safe version).
 * @param conn Connection to check.
 * @return ConnStatusType from libpq.
 */
ConnStatusType pgconn_status_safe(pgconn_t* conn);

/**
 * Gets the timestamp of the last activity on this connection.
 * @param conn Connection to query.
 * @return Unix timestamp of last activity, or 0 if conn is NULL.
 * @note Not thread-safe.
 */
time_t pgconn_last_activity(pgconn_t* conn);

/**
 * Gets the timestamp of the last activity (thread-safe version).
 * @param conn Connection to query.
 * @return Unix timestamp of last activity, or 0 if conn is NULL.
 */
time_t pgconn_last_activity_safe(pgconn_t* conn);

/**
 * Gets the unique connection ID for debugging.
 * @param conn Connection to query.
 * @return Connection ID, or 0 if conn is NULL.
 * @note Not thread-safe.
 */
uint32_t pgconn_connection_id(pgconn_t* conn);

/**
 * Gets the unique connection ID (thread-safe version).
 * @param conn Connection to query.
 * @return Connection ID, or 0 if conn is NULL.
 */
uint32_t pgconn_connection_id_safe(pgconn_t* conn);

// === Manual Locking (for advanced use cases) ===

/**
 * Manually locks the connection for exclusive access.
 * @param conn Connection to lock.
 * @note Only available if connection was created with thread_safe=true.
 * @note Must be paired with pgconn_unlock().
 */
void pgconn_lock(pgconn_t* conn);

/**
 * Manually unlocks the connection.
 * @param conn Connection to unlock.
 * @note Only call after pgconn_lock().
 */
void pgconn_unlock(pgconn_t* conn);

/**
 * Attempts to lock the connection without blocking.
 * @param conn Connection to lock.
 * @return true if lock acquired, false if already locked.
 * @note Only available if connection was created with thread_safe=true.
 */
bool pgconn_trylock(pgconn_t* conn);

#ifdef __cplusplus
}
#endif

#endif  // PGCONN_H
