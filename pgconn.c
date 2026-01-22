#include "pgconn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

// Error message buffer capacity
#define PGCONN_ERR_CAPACITY 512

// Global connection ID counter (atomic-like increment under mutex during creation)
static uint32_t g_next_conn_id = 1;

/** PostgreSQL connection wrapper structure. */
struct pgconn {
    PGconn* raw_conn;                      // Underlying libpq connection
    pthread_mutex_t lock;                  // Mutex for thread-safe operations
    char last_error[PGCONN_ERR_CAPACITY];  // Last error message
    uint32_t connection_id;                // Unique connection identifier
    time_t last_activity;                  // Last query execution time
    int reconnect_attempts;                // Current reconnection attempts
    bool thread_safe;                      // Whether thread-safety is enabled
    bool transaction_active;               // Transaction state flag
    pgconn_config_t config;                // Configuration (with copied strings)
};

// Default query options
static const pgconn_query_opts_t DEFAULT_QUERY_OPTS = {
  .timeout_ms = -1,  // Infinite timeout
  .retry_on_failure = false,
};

// === Internal Helper Functions ===

/** Sets the error message for a connection. */
static void set_error(pgconn_t* conn, const char* message) {
    if (!conn) return;

    if (!message || message[0] == '\0') {
        conn->last_error[0] = '\0';
        return;
    }

    snprintf(conn->last_error, sizeof(conn->last_error), "%s", message);
}

/** Updates the last activity timestamp. */
static inline void update_activity(pgconn_t* conn) {
    if (conn) {
        conn->last_activity = time(NULL);
    }
}

/** Consumes all pending results from the connection. */
static void consume_results(pgconn_t* conn) {
    if (!conn || !conn->raw_conn) return;

    PGresult* res;
    while ((res = PQgetResult(conn->raw_conn)) != NULL) {
        PQclear(res);
    }
}

/** Waits for query completion with optional timeout. */
static bool wait_for_result(pgconn_t* conn, int timeout_ms) {
    if (!conn || !conn->raw_conn) {
        set_error(conn, "Invalid connection");
        return false;
    }

    int socket_fd = PQsocket(conn->raw_conn);
    if (socket_fd < 0) {
        set_error(conn, "Invalid socket file descriptor");
        return false;
    }

    fd_set read_fds;
    struct timeval tv;
    struct timeval* tv_ptr = NULL;

    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tv_ptr = &tv;
    }

    while (true) {
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        int result = select(socket_fd + 1, &read_fds, NULL, NULL, tv_ptr);

        if (result == 0) {
            // Timeout occurred
            set_error(conn, "Query execution timed out");

            // Attempt to cancel the query
            PGcancel* cancel = PQgetCancel(conn->raw_conn);
            if (cancel) {
                char cancel_err[256];
                PQcancel(cancel, cancel_err, sizeof(cancel_err));
                PQfreeCancel(cancel);
            }

            return false;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, retry
            }

            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "select() failed: %s", strerror(errno));
            set_error(conn, err_buf);
            return false;
        }

        // Data available, consume input
        if (PQconsumeInput(conn->raw_conn) == 0) {
            set_error(conn, PQerrorMessage(conn->raw_conn));
            return false;
        }

        if (PQisBusy(conn->raw_conn) == 0) {
            return true;  // Query completed
        }
    }
}

/** Internal connection creation without locking. */
static PGconn* create_raw_connection(const pgconn_config_t* config) {
    if (!config || !config->conninfo) {
        fprintf(stderr, "pgconn: Invalid configuration\n");
        return NULL;
    }

    PGconn* raw = PQconnectdb(config->conninfo);
    if (!raw) {
        fprintf(stderr, "pgconn: PQconnectdb failed to allocate connection\n");
        return NULL;
    }

    if (PQstatus(raw) != CONNECTION_OK) {
        fprintf(stderr, "pgconn: Connection failed: %s\n", PQerrorMessage(raw));
        PQfinish(raw);
        return NULL;
    }

    // Call initialization callback if provided
    if (config->connection_init) {
        config->connection_init(raw);
    }

    return raw;
}

// === Connection Management ===

pgconn_t* pgconn_create(const pgconn_config_t* config) {
    if (!config || !config->conninfo) {
        fprintf(stderr, "pgconn: config and config->conninfo must not be NULL\n");
        return NULL;
    }

    pgconn_t* conn = calloc(1, sizeof(pgconn_t));
    if (!conn) {
        fprintf(stderr, "pgconn: Memory allocation failed\n");
        return NULL;
    }

    // Copy configuration
    conn->config = *config;
    conn->config.conninfo = strdup(config->conninfo);
    if (!conn->config.conninfo) {
        fprintf(stderr, "pgconn: Failed to copy connection string\n");
        free(conn);
        return NULL;
    }

    conn->thread_safe = config->thread_safe;

    // Initialize mutex if thread-safe mode is enabled
    if (conn->thread_safe) {
        if (pthread_mutex_init(&conn->lock, NULL) != 0) {
            fprintf(stderr, "pgconn: Failed to initialize mutex\n");
            free((void*)conn->config.conninfo);
            free(conn);
            return NULL;
        }
    }

    // Assign unique connection ID
    conn->connection_id = __atomic_fetch_add(&g_next_conn_id, 1, __ATOMIC_RELAXED);

    // Create the actual connection
    conn->raw_conn = create_raw_connection(config);
    if (!conn->raw_conn) {
        if (conn->thread_safe) {
            pthread_mutex_destroy(&conn->lock);
        }
        free((void*)conn->config.conninfo);
        free(conn);
        return NULL;
    }

    conn->last_activity = time(NULL);

    return conn;
}

void pgconn_destroy(pgconn_t* conn) {
    if (!conn) return;

    // Rollback active transaction if any
    if (conn->transaction_active && conn->raw_conn) {
        PGresult* res = PQexec(conn->raw_conn, "ROLLBACK");
        PQclear(res);
    }

    // Call close callback if provided
    if (conn->config.connection_close && conn->raw_conn) {
        conn->config.connection_close(conn->raw_conn);
    }

    if (conn->raw_conn) {
        PQfinish(conn->raw_conn);
        conn->raw_conn = NULL;
    }

    free((void*)conn->config.conninfo);

    if (conn->thread_safe) {
        pthread_mutex_destroy(&conn->lock);
    }

    free(conn);
}

PGconn* pgconn_get_raw(pgconn_t* conn) {
    return conn ? conn->raw_conn : NULL;
}

bool pgconn_validate(pgconn_t* conn) {
    if (!conn || !conn->raw_conn) {
        return false;
    }

    if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
        return false;
    }

    // Simple validation query
    PGresult* res = PQexec(conn->raw_conn, "SELECT 1");
    if (!res) {
        return false;
    }

    ExecStatusType status = PQresultStatus(res);
    PQclear(res);

    return (status == PGRES_TUPLES_OK);
}

bool pgconn_validate_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_validate(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

bool pgconn_reconnect(pgconn_t* conn) {
    if (!conn) {
        return false;
    }

    // Check reconnection limits
    if (conn->config.max_reconnect_attempts > 0 && conn->reconnect_attempts >= conn->config.max_reconnect_attempts) {
        set_error(conn, "Maximum reconnection attempts exceeded");
        return false;
    }

    // Close existing connection
    if (conn->raw_conn) {
        if (conn->config.connection_close) {
            conn->config.connection_close(conn->raw_conn);
        }
        PQfinish(conn->raw_conn);
        conn->raw_conn = NULL;
    }

    // Reset transaction state
    conn->transaction_active = false;
    conn->reconnect_attempts++;

    // Create new connection
    conn->raw_conn = create_raw_connection(&conn->config);
    if (!conn->raw_conn) {
        return false;
    }

    conn->reconnect_attempts = 0;  // Reset on success
    conn->last_activity = time(NULL);

    return true;
}

bool pgconn_reconnect_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_reconnect(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Simple Query Execution ===

bool pgconn_execute(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts) {
    if (!conn || !conn->raw_conn || !query) {
        set_error(conn, "Invalid connection or query");
        return false;
    }

    if (!opts) {
        opts = &DEFAULT_QUERY_OPTS;
    }

    consume_results(conn);
    set_error(conn, NULL);

    // Use PQexec for simplicity when no timeout
    if (opts->timeout_ms < 0) {
        PGresult* res = PQexec(conn->raw_conn, query);
        if (!res) {
            set_error(conn, "No result received from query");
            return false;
        }

        ExecStatusType status = PQresultStatus(res);
        bool success = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);

        if (!success) {
            set_error(conn, PQresultErrorMessage(res));
        }

        PQclear(res);
        update_activity(conn);
        return success;
    }

    // With timeout, use async API
    if (PQsendQuery(conn->raw_conn, query) != 1) {
        set_error(conn, PQerrorMessage(conn->raw_conn));
        return false;
    }

    if (!wait_for_result(conn, opts->timeout_ms)) {
        return false;
    }

    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error(conn, "No result received from query");
        return false;
    }

    ExecStatusType status = PQresultStatus(res);
    bool success = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);

    if (!success) {
        set_error(conn, PQresultErrorMessage(res));
    }

    PQclear(res);
    consume_results(conn);
    update_activity(conn);

    return success;
}

bool pgconn_execute_safe(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_execute(conn, query, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

PGresult* pgconn_query(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts) {
    if (!conn || !conn->raw_conn || !query) {
        set_error(conn, "Invalid connection or query");
        return NULL;
    }

    if (!opts) {
        opts = &DEFAULT_QUERY_OPTS;
    }

    consume_results(conn);
    set_error(conn, NULL);

    // Use PQexec for simplicity when no timeout
    if (opts->timeout_ms < 0) {
        PGresult* res = PQexec(conn->raw_conn, query);
        if (!res) {
            set_error(conn, "No result received from query");
            return NULL;
        }

        ExecStatusType status = PQresultStatus(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            set_error(conn, PQresultErrorMessage(res));
            PQclear(res);
            res = NULL;
        }

        update_activity(conn);
        return res;
    }

    // With timeout, use async API
    if (PQsendQuery(conn->raw_conn, query) != 1) {
        set_error(conn, PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!wait_for_result(conn, opts->timeout_ms)) {
        return NULL;
    }

    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error(conn, "No result received from query");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_error(conn, PQresultErrorMessage(res));
        PQclear(res);
        res = NULL;
    }

    consume_results(conn);
    update_activity(conn);

    return res;
}

PGresult* pgconn_query_safe(pgconn_t* conn, const char* query, const pgconn_query_opts_t* opts) {
    if (!conn) return NULL;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    PGresult* result = pgconn_query(conn, query, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Parameterized Query Execution ===

PGresult* pgconn_query_params_full(pgconn_t* conn, const char* query, int n_params, const Oid* param_types,
                                   const char* const* param_values, const int* param_lengths, const int* param_formats,
                                   int result_format, const pgconn_query_opts_t* opts) {
    if (!conn || !conn->raw_conn || !query) {
        set_error(conn, "Invalid connection or query");
        return NULL;
    }

    if (!opts) {
        opts = &DEFAULT_QUERY_OPTS;
    }

    // Normalize parameters
    if (n_params < 0) n_params = 0;

    consume_results(conn);
    set_error(conn, NULL);

    // Use PQexecParams for simplicity when no timeout
    if (opts->timeout_ms < 0) {
        PGresult* res = PQexecParams(
            conn->raw_conn, query, n_params, param_types, param_values, param_lengths, param_formats, result_format);
        if (!res) {
            set_error(conn, "No result received from parameterized query");
            return NULL;
        }

        ExecStatusType status = PQresultStatus(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            set_error(conn, PQresultErrorMessage(res));
            PQclear(res);
            res = NULL;
        }

        update_activity(conn);
        return res;
    }

    // With timeout, use async API
    if (PQsendQueryParams(
            conn->raw_conn, query, n_params, param_types, param_values, param_lengths, param_formats, result_format) !=
        1) {
        set_error(conn, PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!wait_for_result(conn, opts->timeout_ms)) {
        return NULL;
    }

    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error(conn, "No result received from parameterized query");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_error(conn, PQresultErrorMessage(res));
        PQclear(res);
        res = NULL;
    }

    consume_results(conn);
    update_activity(conn);

    return res;
}

PGresult* pgconn_query_params_full_safe(pgconn_t* conn, const char* query, int n_params, const Oid* param_types,
                                        const char* const* param_values, const int* param_lengths,
                                        const int* param_formats, int result_format, const pgconn_query_opts_t* opts) {
    if (!conn) return NULL;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    PGresult* result = pgconn_query_params_full(
        conn, query, n_params, param_types, param_values, param_lengths, param_formats, result_format, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

PGresult* pgconn_query_params(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                              const pgconn_query_opts_t* opts) {
    // Simplified version: assumes all params are null-terminated text strings
    // and result is requested as text.
    return pgconn_query_params_full(conn, query, n_params, NULL, param_values, NULL, NULL, 0, opts);
}

PGresult* pgconn_query_params_safe(pgconn_t* conn, const char* query, int n_params, const char* const* param_values,
                                   const pgconn_query_opts_t* opts) {
    if (!conn) return NULL;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    PGresult* result = pgconn_query_params(conn, query, n_params, param_values, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Prepared Statements ===

bool pgconn_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params, const Oid* param_types) {
    if (!conn || !conn->raw_conn || !stmt_name || !query) {
        set_error(conn, "Invalid connection, statement name, or query");
        return false;
    }

    consume_results(conn);
    set_error(conn, NULL);

    PGresult* res = PQprepare(conn->raw_conn, stmt_name, query, n_params, param_types);
    if (!res) {
        set_error(conn, "No result received from prepare");
        return false;
    }

    bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!success) {
        set_error(conn, PQresultErrorMessage(res));
    }

    PQclear(res);
    update_activity(conn);

    return success;
}

bool pgconn_prepare_safe(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                         const Oid* param_types) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_prepare(conn, stmt_name, query, n_params, param_types);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

PGresult* pgconn_execute_prepared_full(pgconn_t* conn, const char* stmt_name, int n_params,
                                       const char* const* param_values, const int* param_lengths,
                                       const int* param_formats, int result_format, const pgconn_query_opts_t* opts) {
    if (!conn || !conn->raw_conn || !stmt_name) {
        set_error(conn, "Invalid connection or statement name");
        return NULL;
    }

    if (!opts) {
        opts = &DEFAULT_QUERY_OPTS;
    }

    consume_results(conn);
    set_error(conn, NULL);

    // Use PQexecPrepared for simplicity when no timeout
    if (opts->timeout_ms < 0) {
        PGresult* res = PQexecPrepared(
            conn->raw_conn, stmt_name, n_params, param_values, param_lengths, param_formats, result_format);
        if (!res) {
            set_error(conn, "No result received from prepared statement");
            return NULL;
        }

        ExecStatusType status = PQresultStatus(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            set_error(conn, PQresultErrorMessage(res));
            PQclear(res);
            res = NULL;
        }

        update_activity(conn);
        return res;
    }

    // With timeout, use async API
    if (PQsendQueryPrepared(
            conn->raw_conn, stmt_name, n_params, param_values, param_lengths, param_formats, result_format) != 1) {
        set_error(conn, PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    if (!wait_for_result(conn, opts->timeout_ms)) {
        return NULL;
    }

    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error(conn, "No result received from prepared statement");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_error(conn, PQresultErrorMessage(res));
        PQclear(res);
        res = NULL;
    }

    consume_results(conn);
    update_activity(conn);

    return res;
}

PGresult* pgconn_execute_prepared_full_safe(pgconn_t* conn, const char* stmt_name, int n_params,
                                            const char* const* param_values, const int* param_lengths,
                                            const int* param_formats, int result_format,
                                            const pgconn_query_opts_t* opts) {
    if (!conn) return NULL;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    PGresult* result = pgconn_execute_prepared_full(
        conn, stmt_name, n_params, param_values, param_lengths, param_formats, result_format, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

PGresult* pgconn_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params, const char* const* param_values,
                                  const pgconn_query_opts_t* opts) {
    // Simplified version: assumes all params are null-terminated text strings
    // and result is requested as text.
    return pgconn_execute_prepared_full(conn, stmt_name, n_params, param_values, NULL, NULL, 0, opts);
}

PGresult* pgconn_execute_prepared_safe(pgconn_t* conn, const char* stmt_name, int n_params,
                                       const char* const* param_values, const pgconn_query_opts_t* opts) {
    if (!conn) return NULL;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    PGresult* result = pgconn_execute_prepared(conn, stmt_name, n_params, param_values, opts);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

bool pgconn_deallocate(pgconn_t* conn, const char* stmt_name) {
    if (!conn || !conn->raw_conn || !stmt_name) {
        set_error(conn, "Invalid connection or statement name");
        return false;
    }

    set_error(conn, NULL);

    // Build DEALLOCATE command
    char query[512];
    snprintf(query, sizeof(query), "DEALLOCATE %s", stmt_name);

    bool result = pgconn_execute(conn, query, NULL);
    return result;
}

bool pgconn_deallocate_safe(pgconn_t* conn, const char* stmt_name) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_deallocate(conn, stmt_name);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Transaction Management ===

bool pgconn_begin(pgconn_t* conn) {
    if (!conn) {
        return false;
    }

    if (conn->transaction_active) {
        set_error(conn, "Transaction already active");
        return false;
    }

    bool result = pgconn_execute(conn, "BEGIN", NULL);
    if (result) {
        conn->transaction_active = true;
    }

    return result;
}

bool pgconn_begin_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_begin(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

bool pgconn_commit(pgconn_t* conn) {
    if (!conn) {
        return false;
    }

    if (!conn->transaction_active) {
        set_error(conn, "No active transaction to commit");
        return false;
    }

    bool result = pgconn_execute(conn, "COMMIT", NULL);
    conn->transaction_active = false;

    return result;
}

bool pgconn_commit_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_commit(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

bool pgconn_rollback(pgconn_t* conn) {
    if (!conn) {
        return false;
    }

    if (!conn->transaction_active) {
        set_error(conn, "No active transaction to rollback");
        return false;
    }

    bool result = pgconn_execute(conn, "ROLLBACK", NULL);
    conn->transaction_active = false;

    return result;
}

bool pgconn_rollback_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = pgconn_rollback(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

bool pgconn_in_transaction(pgconn_t* conn) {
    return conn ? conn->transaction_active : false;
}

bool pgconn_in_transaction_safe(pgconn_t* conn) {
    if (!conn) return false;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    bool result = conn->transaction_active;

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Error Handling ===

const char* pgconn_error_message(pgconn_t* conn) {
    if (!conn) {
        return "Invalid connection";
    }

    if (conn->last_error[0]) {
        return conn->last_error;
    }

    if (conn->raw_conn) {
        const char* pg_error = PQerrorMessage(conn->raw_conn);
        if (pg_error && pg_error[0]) {
            return pg_error;
        }
    }

    return "No error information available";
}

const char* pgconn_error_message_safe(pgconn_t* conn) {
    if (!conn) {
        return "Invalid connection";
    }

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    const char* result = pgconn_error_message(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

void pgconn_clear_error(pgconn_t* conn) {
    if (conn) {
        conn->last_error[0] = '\0';
    }
}

void pgconn_clear_error_safe(pgconn_t* conn) {
    if (!conn) return;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    conn->last_error[0] = '\0';

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }
}

// === Connection State ===

ConnStatusType pgconn_status(pgconn_t* conn) {
    if (!conn || !conn->raw_conn) {
        return CONNECTION_BAD;
    }

    return PQstatus(conn->raw_conn);
}

ConnStatusType pgconn_status_safe(pgconn_t* conn) {
    if (!conn) {
        return CONNECTION_BAD;
    }

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    ConnStatusType result = pgconn_status(conn);

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

time_t pgconn_last_activity(pgconn_t* conn) {
    return conn ? conn->last_activity : 0;
}

time_t pgconn_last_activity_safe(pgconn_t* conn) {
    if (!conn) return 0;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    time_t result = conn->last_activity;

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

uint32_t pgconn_connection_id(pgconn_t* conn) {
    return conn ? conn->connection_id : 0;
}

uint32_t pgconn_connection_id_safe(pgconn_t* conn) {
    if (!conn) return 0;

    if (conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }

    uint32_t result = conn->connection_id;

    if (conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }

    return result;
}

// === Manual Locking ===

void pgconn_lock(pgconn_t* conn) {
    if (conn && conn->thread_safe) {
        pthread_mutex_lock(&conn->lock);
    }
}

void pgconn_unlock(pgconn_t* conn) {
    if (conn && conn->thread_safe) {
        pthread_mutex_unlock(&conn->lock);
    }
}

bool pgconn_trylock(pgconn_t* conn) {
    if (!conn || !conn->thread_safe) {
        return false;
    }

    return pthread_mutex_trylock(&conn->lock) == 0;
}
