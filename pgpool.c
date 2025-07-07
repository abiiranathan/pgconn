#include "pgpool.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// Internal connection wrapper structure
struct pgconn {
    PGconn* raw_conn;         // The actual libpq connection
    bool in_use;              // Flag indicating if connection is in use
    bool transaction_active;  // Flag for transaction state
    time_t last_activity;     // Last time the connection was used
    char* last_error;         // Last error message
    uint32_t connection_id;   // Unique connection identifier for debugging
};

// Connection pool structure
struct pgpool {
    pthread_mutex_t lock;  // Mutex for thread safety
    pthread_cond_t cond;   // Condition variable for waiting threads

    pgconn_t** connections;   // Array of all connections
    size_t connection_count;  // Current number of connections
    size_t idle_count;        // Number of idle connections

    pgpool_config_t config;  // Configuration parameters
    char* conninfo;          // Copy of connection string

    bool shutting_down;     // Flag indicating pool is being destroyed
    bool initialized;       // Flag indicating pool is properly initialized
    uint32_t next_conn_id;  // Next connection ID to assign
};

// Default configuration values
static const pgpool_config_t DEFAULT_CONFIG = {
    .min_connections = 1,
    .max_connections = 10,
    .connect_timeout = 5,
    .auto_reconnect = true,
    .connection_init = NULL,
    .connection_close = NULL,
    .conninfo = NULL,
};

// Safe string duplication with error handling
static inline char* safe_strdup(const char* str) {
    if (!str)
        return NULL;

    char* copy = strdup(str);
    if (!copy) {
        fprintf(stderr, "pgpool: Memory allocation failed in safe_strdup\n");
    }
    return copy;
}

// Safe error message setting
static void set_error_message(pgconn_t* conn, const char* message) {
    if (!conn)
        return;

    free(conn->last_error);
    conn->last_error = NULL;

    if (message) {
        conn->last_error = safe_strdup(message);
    }
}

// Internal function to create a new connection
static pgconn_t* create_connection(pgpool_t* pool) {
    if (!pool || !pool->conninfo) {
        fprintf(stderr, "pgpool: Invalid pool or missing connection info\n");
        return NULL;
    }

    pgconn_t* conn = calloc(1, sizeof(pgconn_t));
    if (!conn) {
        fprintf(stderr, "pgpool: Memory allocation failed for connection\n");
        return NULL;
    }

    conn->connection_id = ++pool->next_conn_id;

    // Create the actual PostgreSQL connection
    conn->raw_conn = PQconnectdb(pool->conninfo);
    if (!conn->raw_conn) {
        fprintf(stderr, "pgpool: PQconnectdb failed to allocate connection\n");
        free(conn);
        return NULL;
    }

    if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
        set_error_message(conn, PQerrorMessage(conn->raw_conn));
        fprintf(stderr, "pgpool: Connection failed (ID: %u): %s\n", conn->connection_id,
                conn->last_error ? conn->last_error : "Unknown error");
        PQfinish(conn->raw_conn);
        free(conn);
        return NULL;
    }

    // Set non-blocking mode
    if (PQsetnonblocking(conn->raw_conn, 1) != 0) {
        set_error_message(conn, "Failed to set non-blocking mode");
        fprintf(stderr, "pgpool: Failed to set non-blocking mode (ID: %u)\n", conn->connection_id);
        PQfinish(conn->raw_conn);
        free(conn);
        return NULL;
    }

    // Set connection timeout if specified
    if (pool->config.connect_timeout > 0) {
        char timeout_str[32];
        snprintf(timeout_str, sizeof(timeout_str), "SET statement_timeout = %d",
                 pool->config.connect_timeout * 1000);

        PGresult* res = PQexec(conn->raw_conn, timeout_str);
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "pgpool: Failed to set timeout (ID: %u)\n", conn->connection_id);
            // Don't fail connection creation for this
        }
        PQclear(res);
    }

    // Call initialization callback if provided
    if (pool->config.connection_init) {
        pool->config.connection_init(conn->raw_conn);
    }

    conn->last_activity = time(NULL);
    return conn;
}

// Internal function to validate a connection
static bool validate_connection(pgconn_t* conn) {
    if (!conn || !conn->raw_conn) {
        return false;
    }

    // Check connection status first
    if (PQstatus(conn->raw_conn) != CONNECTION_OK) {
        return false;
    }

    // Simple validation query with timeout
    PGresult* res = PQexec(conn->raw_conn, "SELECT 1");
    if (!res) {
        return false;
    }

    ExecStatusType status = PQresultStatus(res);
    PQclear(res);

    return (status == PGRES_TUPLES_OK);
}

// Internal function to destroy a connection
static void destroy_connection(pgpool_t* pool, pgconn_t* conn) {
    if (!conn)
        return;

    // Call close callback if provided
    if (pool && pool->config.connection_close && conn->raw_conn) {
        pool->config.connection_close(conn->raw_conn);
    }

    if (conn->raw_conn) {
        PQfinish(conn->raw_conn);
        conn->raw_conn = NULL;
    }

    free(conn->last_error);
    conn->last_error = NULL;

    free(conn);
}

// Create a new connection pool
pgpool_t* pgpool_create(const pgpool_config_t* config) {
    if (!config || !config->conninfo) {
        fprintf(stderr, "pgpool: config and config->conninfo must not be NULL\n");
        return NULL;
    }

    // Validate configuration parameters
    if (config->max_connections < 1 || config->min_connections > config->max_connections) {
        fprintf(stderr, "pgpool: Invalid connection pool configuration\n");
        return NULL;
    }

    pgpool_t* pool = calloc(1, sizeof(pgpool_t));
    if (!pool) {
        fprintf(stderr, "pgpool: Memory allocation failed for pool\n");
        return NULL;
    }

    // Initialize with default config then override with user settings
    pool->config = DEFAULT_CONFIG;
    if (config->min_connections > 0)
        pool->config.min_connections = config->min_connections;
    if (config->max_connections > 0)
        pool->config.max_connections = config->max_connections;
    if (config->connect_timeout > 0)
        pool->config.connect_timeout = config->connect_timeout;
    if (config->auto_reconnect)
        pool->config.auto_reconnect = config->auto_reconnect;
    if (config->connection_init)
        pool->config.connection_init = config->connection_init;
    if (config->connection_close)
        pool->config.connection_close = config->connection_close;

    // Make a copy of the connection string
    pool->conninfo = safe_strdup(config->conninfo);
    if (!pool->conninfo) {
        goto error_free_pool;
    }

    // Initialize synchronization primitives
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        fprintf(stderr, "pgpool: Failed to initialize mutex\n");
        goto error_free_conninfo;
    }

    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        fprintf(stderr, "pgpool: Failed to initialize condition variable\n");
        goto error_destroy_mutex;
    }

    // Allocate connection array
    pool->connections = calloc(pool->config.max_connections, sizeof(pgconn_t*));
    if (!pool->connections) {
        fprintf(stderr, "pgpool: Memory allocation failed for connections array\n");
        goto error_destroy_cond;
    }

    // Pre-create minimum connections
    for (size_t i = 0; i < (size_t)pool->config.min_connections; i++) {
        pgconn_t* conn = create_connection(pool);
        if (conn) {
            pool->connections[pool->connection_count++] = conn;
            pool->idle_count++;
        } else {
            fprintf(stderr, "pgpool: Failed to create initial connection %zu\n", i);
            // Continue with fewer connections rather than failing completely
        }
    }

    // We failed to create any connections.
    if (pool->idle_count == 0) {
        fprintf(stderr, "pgpool: failed to initialize any connections\n");
        goto error_free_connections;
    }

    pool->initialized = true;
    return pool;

// Error handling with goto
error_free_connections:
    free(pool->connections);
error_destroy_cond:
    pthread_cond_destroy(&pool->cond);
error_destroy_mutex:
    pthread_mutex_destroy(&pool->lock);
error_free_conninfo:
    free(pool->conninfo);
error_free_pool:
    free(pool);
    return NULL;
}

// Destroy a connection pool
void pgpool_destroy(pgpool_t* pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);

    if (!pool->initialized) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->cond);

    // Wait for all connections to be released
    int wait_count = 0;
    while (pool->idle_count < pool->connection_count && wait_count < 10) {
        pthread_mutex_unlock(&pool->lock);
        usleep(100000);  // 100ms
        pthread_mutex_lock(&pool->lock);
        wait_count++;
    }

    if (pool->idle_count < pool->connection_count) {
        fprintf(stderr, "pgpool: Warning - destroying pool with %zu active connections\n",
                pool->connection_count - pool->idle_count);
    }

    // Destroy all connections
    for (size_t i = 0; i < pool->connection_count; i++) {
        destroy_connection(pool, pool->connections[i]);
        pool->connections[i] = NULL;
    }

    free(pool->connections);
    pool->connections = NULL;

    free(pool->conninfo);
    pool->conninfo = NULL;

    pool->initialized = false;
    pthread_mutex_unlock(&pool->lock);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool);
}

// Acquire a connection from the pool
pgconn_t* pgpool_acquire(pgpool_t* pool, int timeout_ms) {
    if (!pool || !pool->initialized) {
        fprintf(stderr, "pgpool: Invalid or uninitialized pool\n");
        return NULL;
    }

    struct timespec ts;
    bool use_timeout = (timeout_ms > 0);

    if (use_timeout) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) {
            fprintf(stderr, "pgpool: gettimeofday failed\n");
            return NULL;
        }

        ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
        ts.tv_nsec = tv.tv_usec * 1000 + (timeout_ms % 1000) * 1000000L;

        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec++;
        }
    }

    pthread_mutex_lock(&pool->lock);

    while (!pool->shutting_down) {
        // First try to find an idle connection
        for (size_t i = 0; i < pool->connection_count; i++) {
            pgconn_t* conn = pool->connections[i];
            if (!conn || conn->in_use)
                continue;

            // Validate the connection if auto_reconnect is enabled
            if (pool->config.auto_reconnect && !validate_connection(conn)) {
                fprintf(stderr, "pgpool: Reconnecting stale connection (ID: %u)\n", conn->connection_id);
                destroy_connection(pool, conn);
                pool->connections[i] = create_connection(pool);
                conn = pool->connections[i];
                if (!conn) {
                    // Remove the slot and compact the array
                    for (size_t j = i; j < pool->connection_count - 1; j++) {
                        pool->connections[j] = pool->connections[j + 1];
                    }
                    pool->connection_count--;
                    pool->idle_count--;
                    continue;
                }
            }

            // Mark connection as in use
            conn->in_use = true;
            conn->last_activity = time(NULL);
            if (pool->idle_count > 0) {
                pool->idle_count--;
            }

            pthread_mutex_unlock(&pool->lock);
            return conn;
        }

        // If we have room to create a new connection, do so
        if (pool->connection_count < (size_t)pool->config.max_connections) {
            pgconn_t* conn = create_connection(pool);
            if (conn) {
                conn->in_use = true;
                conn->last_activity = time(NULL);
                pool->connections[pool->connection_count++] = conn;
                pthread_mutex_unlock(&pool->lock);
                return conn;
            }
        }

        // No connections available, wait for one to be released
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        } else if (timeout_ms < 0) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        } else {
            int wait_result = pthread_cond_timedwait(&pool->cond, &pool->lock, &ts);
            if (wait_result == ETIMEDOUT) {
                pthread_mutex_unlock(&pool->lock);
                return NULL;
            } else if (wait_result != 0) {
                fprintf(stderr, "pgpool: pthread_cond_timedwait failed: %s\n", strerror(wait_result));
                pthread_mutex_unlock(&pool->lock);
                return NULL;
            }
        }
    }

    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

// Release a connection back to the pool
void pgpool_release(pgpool_t* pool, pgconn_t* conn) {
    if (!pool || !conn || !pool->initialized) {
        fprintf(stderr, "pgpool: Invalid pool or connection in release\n");
        return;
    }

    pthread_mutex_lock(&pool->lock);

    // Verify this connection belongs to this pool
    bool found = false;
    for (size_t i = 0; i < pool->connection_count; i++) {
        if (pool->connections[i] == conn) {
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "pgpool: Attempted to release connection not owned by pool\n");
        pthread_mutex_unlock(&pool->lock);
        return;
    }

    // Reset transaction state if needed
    if (conn->transaction_active) {
        fprintf(stderr, "pgpool: Warning - releasing connection with active transaction (ID: %u)\n",
                conn->connection_id);
        // Attempt to rollback
        PGresult* res = PQexec(conn->raw_conn, "ROLLBACK");
        PQclear(res);
        conn->transaction_active = false;
    }

    conn->in_use = false;
    conn->last_activity = time(NULL);
    pool->idle_count++;

    // Clear any previous error
    free(conn->last_error);
    conn->last_error = NULL;

    // Signal waiting threads that a connection is available
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

// Clear any pending results from a connection
static void consume_all_results(pgconn_t* conn) {
    if (!conn || !conn->raw_conn)
        return;

    PGresult* res;
    while ((res = PQgetResult(conn->raw_conn)) != NULL) {
        PQclear(res);
    }
}

// Wait for query completion with timeout
static bool wait_for_query_completion(pgconn_t* conn, int timeout_ms) {
    if (!conn || !conn->raw_conn)
        return false;

    int socket_fd = PQsocket(conn->raw_conn);
    if (socket_fd < 0) {
        set_error_message(conn, "Invalid socket file descriptor");
        return false;
    }

    fd_set input_mask;
    struct timeval tv;

    while (true) {
        FD_ZERO(&input_mask);
        FD_SET(socket_fd, &input_mask);

        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
        }

        int result = select(socket_fd + 1, &input_mask, NULL, NULL, timeout_ms >= 0 ? &tv : NULL);

        if (result == 0) {  // Timeout
            set_error_message(conn, "Query execution timed out");
            PQcancel(PQgetCancel(conn->raw_conn), NULL, 0);
            return false;
        } else if (result < 0) {  // Error
            if (errno == EINTR)
                continue;  // Interrupted by signal, retry

            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf), "select() failed: %s", strerror(errno));
            set_error_message(conn, error_buf);
            return false;
        }

        // Data available, consume it
        if (PQconsumeInput(conn->raw_conn) == 0) {
            set_error_message(conn, PQerrorMessage(conn->raw_conn));
            return false;
        }

        if (PQisBusy(conn->raw_conn) == 0) {
            return true;  // Query completed
        }
    }
}

// Execute a query using a pooled connection
bool pgpool_execute(pgconn_t* conn, const char* query, int timeout_ms) {
    if (!conn || !conn->raw_conn || !query) {
        if (conn)
            set_error_message(conn, "Invalid connection or query");
        return false;
    }

    // Clear any previous results
    consume_all_results(conn);
    set_error_message(conn, NULL);

    // Send the query
    if (PQsendQuery(conn->raw_conn, query) != 1) {
        set_error_message(conn, PQerrorMessage(conn->raw_conn));
        return false;
    }

    // Wait for completion
    if (!wait_for_query_completion(conn, timeout_ms)) {
        return false;
    }

    // Check result status
    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error_message(conn, "No result received from query");
        return false;
    }

    ExecStatusType status = PQresultStatus(res);
    bool success = (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK);

    if (!success) {
        set_error_message(conn, PQresultErrorMessage(res));
    }

    PQclear(res);

    // Consume any additional results
    consume_all_results(conn);

    return success;
}

// Execute a query and return the result to the caller
PGresult* pgpool_query(pgconn_t* conn, const char* query, int timeout_ms) {
    if (!conn || !conn->raw_conn || !query) {
        if (conn)
            set_error_message(conn, "Invalid connection or query");
        return NULL;
    }

    // Clear any previous results
    consume_all_results(conn);
    set_error_message(conn, NULL);

    // Send the query
    if (PQsendQuery(conn->raw_conn, query) != 1) {
        set_error_message(conn, PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    // Wait for completion
    if (!wait_for_query_completion(conn, timeout_ms)) {
        return NULL;
    }

    // Get the result
    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error_message(conn, "No result received from query");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        set_error_message(conn, PQresultErrorMessage(res));
        PQclear(res);
        res = NULL;
    }

    // Consume any additional results
    consume_all_results(conn);

    return res;
}

// Prepare a statement
bool pgpool_prepare(pgconn_t* conn, const char* stmt_name, const char* query, int n_params,
                    const Oid* param_types, int timeout_ms) {
    if (!conn || !conn->raw_conn || !stmt_name || !query) {
        if (conn)
            set_error_message(conn, "Invalid connection, statement name, or query");
        return false;
    }

    // Clear any previous results
    consume_all_results(conn);
    set_error_message(conn, NULL);

    // Send the prepare command
    if (PQsendPrepare(conn->raw_conn, stmt_name, query, n_params, param_types) != 1) {
        set_error_message(conn, PQerrorMessage(conn->raw_conn));
        return false;
    }

    // Wait for completion
    if (!wait_for_query_completion(conn, timeout_ms)) {
        return false;
    }

    // Check result
    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error_message(conn, "No result received from prepare");
        return false;
    }

    bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!success) {
        set_error_message(conn, PQresultErrorMessage(res));
    }

    PQclear(res);
    consume_all_results(conn);

    return success;
}

// Execute a prepared statement
PGresult* pgpool_execute_prepared(pgconn_t* conn, const char* stmt_name, int n_params,
                                  const char* const* param_values, const int* param_lengths,
                                  const int* param_formats, int result_format, int timeout_ms) {
    if (!conn || !conn->raw_conn || !stmt_name) {
        if (conn)
            set_error_message(conn, "Invalid connection or statement name");
        return NULL;
    }

    // Clear any previous results
    consume_all_results(conn);
    set_error_message(conn, NULL);

    // Send the execute command
    if (PQsendQueryPrepared(conn->raw_conn, stmt_name, n_params, param_values, param_lengths, param_formats,
                            result_format) != 1) {
        set_error_message(conn, PQerrorMessage(conn->raw_conn));
        return NULL;
    }

    // Wait for completion
    if (!wait_for_query_completion(conn, timeout_ms)) {
        return NULL;
    }

    // Get the result
    PGresult* res = PQgetResult(conn->raw_conn);
    if (!res) {
        set_error_message(conn, "No result received from prepared statement");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        set_error_message(conn, PQresultErrorMessage(res));
        PQclear(res);
        res = NULL;
    }

    consume_all_results(conn);
    return res;
}

// Deallocate a prepared statement
bool pgpool_deallocate(pgconn_t* conn, const char* stmt_name, int timeout_ms) {
    if (!conn || !conn->raw_conn || !stmt_name) {
        if (conn)
            set_error_message(conn, "Invalid connection or statement name");
        return false;
    }

    // Clear previous error
    set_error_message(conn, NULL);

    // Create the deallocate command
    size_t query_len = strlen("DEALLOCATE ") + strlen(stmt_name) + 1;
    char* query = malloc(query_len);
    if (!query) {
        set_error_message(conn, "Memory allocation failed");
        return false;
    }

    snprintf(query, query_len, "DEALLOCATE %s", stmt_name);

    bool result = pgpool_execute(conn, query, timeout_ms);
    free(query);

    return result;
}

// Begin a transaction
bool pgpool_begin(pgconn_t* conn) {
    if (!conn)
        return false;

    if (conn->transaction_active) {
        set_error_message(conn, "Transaction already active");
        return false;
    }

    bool result = pgpool_execute(conn, "BEGIN", -1);
    if (result) {
        conn->transaction_active = true;
    }
    return result;
}

// Commit a transaction
bool pgpool_commit(pgconn_t* conn) {
    if (!conn)
        return false;

    if (!conn->transaction_active) {
        set_error_message(conn, "No active transaction to commit");
        return false;
    }

    bool result = pgpool_execute(conn, "COMMIT", -1);
    conn->transaction_active = false;
    return result;
}

// Rollback a transaction
bool pgpool_rollback(pgconn_t* conn) {
    if (!conn)
        return false;

    if (!conn->transaction_active) {
        set_error_message(conn, "No active transaction to rollback");
        return false;
    }

    bool result = pgpool_execute(conn, "ROLLBACK", -1);
    conn->transaction_active = false;
    return result;
}

// Get the underlying libpq connection
PGconn* pgpool_get_raw_connection(pgconn_t* conn) {
    return conn ? conn->raw_conn : NULL;
}

// Get the last error message
const char* pgpool_error_message(pgconn_t* conn) {
    if (!conn)
        return "Invalid connection";

    if (conn->last_error) {
        return conn->last_error;
    }

    if (conn->raw_conn) {
        const char* pg_error = PQerrorMessage(conn->raw_conn);
        if (pg_error && strlen(pg_error) > 0) {
            return pg_error;
        }
    }

    return "No error information available";
}

// Get number of active connections
size_t pgpool_active_connections(pgpool_t* pool) {
    if (!pool || !pool->initialized)
        return 0;

    pthread_mutex_lock(&pool->lock);
    size_t count = pool->connection_count - pool->idle_count;
    pthread_mutex_unlock(&pool->lock);

    return count;
}

// Get number of idle connections
size_t pgpool_idle_connections(pgpool_t* pool) {
    if (!pool || !pool->initialized)
        return 0;

    pthread_mutex_lock(&pool->lock);
    size_t count = pool->idle_count;
    pthread_mutex_unlock(&pool->lock);

    return count;
}

// Get total number of connections
size_t pgpool_total_connections(pgpool_t* pool) {
    if (!pool || !pool->initialized)
        return 0;

    pthread_mutex_lock(&pool->lock);
    size_t count = pool->connection_count;
    pthread_mutex_unlock(&pool->lock);

    return count;
}

// Check if connection is in transaction
bool pgpool_in_transaction(pgconn_t* conn) {
    return conn ? conn->transaction_active : false;
}

// Get connection ID for debugging
uint32_t pgpool_connection_id(pgconn_t* conn) {
    return conn ? conn->connection_id : 0;
}
