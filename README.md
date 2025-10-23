# pgconn - Thread-Aware PostgreSQL Connection Library

A clean, robust C library wrapping `libpq` with both lock-free and thread-safe APIs for PostgreSQL connections.

## Features

-   **Dual API Design**: Lock-free by default for maximum performance, with explicit `_safe` suffixed functions for thread safety.
-   **Per-Connection Locking**: Each connection has its own mutex (no global locks) when created in thread-safe mode.
-   **Zero Deadlock Risk**: The library never holds multiple connection locks simultaneously.
-   **Manual Locking Support**: Lock once, then execute multiple default (lock-free) operations for high-performance batching.
-   **Query Timeouts**: Built-in support for query execution timeouts.
-   **Auto-Reconnection**: Configurable automatic reconnection on connection loss.
-   **Simplified API**: Ergonomic functions for common use cases (e.g., text-only parameterized queries).
-   **Transaction Management**: `BEGIN`, `COMMIT`, `ROLLBACK` with state tracking.

## Design Philosophy

The core principle is **performance by default**. Locking is an explicit choice you make when you need to share a connection across threads.

### Two Usage Patterns

1.  **Single-threaded or Per-Thread Connections (Recommended)**
    -   Create connections with `thread_safe = false` in the configuration.
    -   Use the **default functions** (e.g., `pgconn_query()`, `pgconn_execute()`) for zero locking overhead.
    -   This model, where each thread owns its own connection, offers the best performance and simplest mental model.

2.  **Shared Connection Across Threads**
    -   Create the connection with `thread_safe = true` in the configuration. This initializes a mutex for the connection object.
    -   Use the **`_safe` suffixed functions** (e.g., `pgconn_query_safe()`) for automatic locking and unlocking.
    -   This is convenient for simple cases but can lead to contention under heavy load.

### Deadlock Avoidance

The library is designed to be deadlock-free:
-   Each connection has its own independent mutex.
-   Thread-safe (`*_safe`) functions always follow a strict pattern: **lock → call default function → unlock**.
-   No function ever attempts to hold locks on multiple connections.

## Building

```bash
# Build the library and examples
make

# Run the examples
make run

# Install the library and header system-wide
sudo make install

# Clean up build artifacts
make clean```

## Quick Start

### Single-Threaded Usage (Default, No Locking)

```c
#include "pgconn.h"

int main() {
    pgconn_config_t config = {
        .conninfo = "host=localhost dbname=mydb user=postgres",
        .connect_timeout = 5,
        .thread_safe = false,  // No locking needed
        .auto_reconnect = true,
    };

    pgconn_t* conn = pgconn_create(&config);
    if (!conn) return 1;

    // Use the default, lock-free API for best performance
    PGresult* res = pgconn_query(conn, "SELECT * FROM users", NULL);
    if (res) {
        // Process results...
        PQclear(res);
    }

    pgconn_destroy(conn);
    return 0;
}
```

### Multi-Threaded with a Shared Connection

```c
// Global shared connection
pgconn_t* g_conn;

void* worker(void* arg) {
    // Use the thread-safe API, which handles locking internally
    PGresult* res = pgconn_query_safe(g_conn, "SELECT NOW()", NULL);
    if (res) {
        printf("Worker thread got time: %s\n", PQgetvalue(res, 0, 0));
        PQclear(res);
    }
    return NULL;
}

int main() {
    pgconn_config_t config = {
        .conninfo = "host=localhost dbname=mydb user=postgres",
        .thread_safe = true,  // IMPORTANT: Enable internal locking
    };

    g_conn = pgconn_create(&config);
    if (!g_conn) return 1;

    // Spawn threads that all use g_conn...
    // pthread_create(...);

    pgconn_destroy_safe(g_conn); // Use safe version for destruction
    return 0;
}```

### Multi-Threaded with Per-Thread Connections (Best Performance)

```c
void* worker(void* arg) {
    // Each thread creates its own dedicated connection
    pgconn_config_t config = {
        .conninfo = "host=localhost dbname=mydb",
        .thread_safe = false,  // Locking is unnecessary
    };
    
    pgconn_t* conn = pgconn_create(&config);
    if (!conn) return NULL;
    
    // Use the default API for maximum performance
    PGresult* res = pgconn_query(conn, "SELECT COUNT(*) FROM users", NULL);
    if (res) {
        // Process results...
        PQclear(res);
    }
    
    pgconn_destroy(conn);
    return NULL;
}
```

## API Overview

Each function has a default lock-free version and an optional `_safe` version for thread-safe execution.

### Connection Management

-   `pgconn_create()`
-   `pgconn_destroy()` / `pgconn_destroy_safe()`
-   `pgconn_get_raw()`
-   `pgconn_validate()` / `pgconn_validate_safe()`
-   `pgconn_reconnect()` / `pgconn_reconnect_safe()`

### Query Execution

-   `pgconn_execute()` / `pgconn_execute_safe()` - Execute, return `bool` success.
-   `pgconn_query()` / `pgconn_query_safe()` - Execute, return `PGresult*`.
-   `pgconn_query_params()` / `pgconn_query_params_safe()` - Simplified parameterized query.
-   `pgconn_query_params_full()` / `pgconn_query_params_full_safe()` - Full-featured parameterized query.

### Prepared Statements

-   `pgconn_prepare()` / `pgconn_prepare_safe()`
-   `pgconn_execute_prepared()` / `pgconn_execute_prepared_safe()` - Simplified execution.
-   `pgconn_execute_prepared_full()` / `pgconn_execute_prepared_full_safe()` - Full-featured execution.
-   `pgconn_deallocate()` / `pgconn_deallocate_safe()`

### Transactions

-   `pgconn_begin()` / `pgconn_begin_safe()`
-   `pgconn_commit()` / `pgconn_commit_safe()`
-   `pgconn_rollback()` / `pgconn_rollback_safe()`
-   `pgconn_in_transaction()` / `pgconn_in_transaction_safe()`

### Error Handling & State

-   `pgconn_error_message()` / `pgconn_error_message_safe()`
-   `pgconn_clear_error()` / `pgconn_clear_error_safe()`
-   `pgconn_status()` / `pgconn_status_safe()`
-   `pgconn_last_activity()` / `pgconn_last_activity_safe()`
-   `pgconn_connection_id()` / `pgconn_connection_id_safe()`

### Manual Locking (Advanced)

-   `pgconn_lock()`
-   `pgconn_unlock()`
-   `pgconn_trylock()`

## Usage Patterns

### Query with Timeout

```c
pgconn_query_opts_t opts = {
    .timeout_ms = 5000,  // 5 second timeout
};

PGresult* res = pgconn_query(conn, "SELECT * FROM large_table", &opts);
if (!res) {
    fprintf(stderr, "Query timed out or failed: %s\n", pgconn_error_message(conn));
}
```

### Transaction with Error Handling

```c
if (!pgconn_begin(conn)) {
    fprintf(stderr, "BEGIN failed: %s\n", pgconn_error_message(conn));
    return;
}

bool success = true;
// ... perform multiple queries
if (!pgconn_execute(conn, "INSERT INTO ...", NULL)) {
    fprintf(stderr, "Insert failed: %s\n", pgconn_error_message(conn));
    success = false;
}

if (success) {
    pgconn_commit(conn);
} else {
    pgconn_rollback(conn);
}
```

### Manual Locking for Batch Operations

This pattern is for thread-safe connections where you want to minimize lock/unlock overhead for a series of operations.

```c
// Lock once for the entire batch
pgconn_lock(conn);

// Use the fast, non-locking functions inside the lock
pgconn_begin(conn);
for (int i = 0; i < 1000; i++) {
    pgconn_execute(conn, query, NULL);
}
pgconn_commit(conn);

// Unlock when the batch is complete
pgconn_unlock(conn);
```

### Prepared Statement Pattern

```c
// 1. Prepare the statement once
const char* stmt_name = "get_user";
const char* query = "SELECT name, email FROM users WHERE id = $1";
pgconn_prepare(conn, stmt_name, query, 1, NULL);

// 2. Execute it many times with different parameters
for (int i = 0; i < 100; i++) {
    char id_str[12];
    snprintf(id_str, sizeof(id_str), "%d", i);
    const char* params[] = {id_str};
    
    // Use the simplified function for text parameters
    PGresult* res = pgconn_execute_prepared(conn, stmt_name, 1, params, NULL);
    if (res) {
        // Process results...
        PQclear(res);
    }
}

// 3. Clean up when done
pgconn_deallocate(conn, stmt_name);
```

## Configuration Options

```c
typedef struct {
    const char* conninfo;             // PostgreSQL connection string (required)
    int connect_timeout;              // Connection timeout in seconds (0 = default)
    bool thread_safe;                 // Enables the internal mutex, allowing _safe functions to be used
    bool auto_reconnect;              // Attempt to reconnect on connection loss
    int max_reconnect_attempts;       // Limit for auto_reconnect (0 = infinite)
    void (*connection_init)(PGconn*); // Callback invoked after a successful connection
    void (*connection_close)(PGconn*);// Callback invoked before a connection is closed
} pgconn_config_t;
```

## Error Handling

Functions that can fail return either `false` (for bools) or `NULL` (for pointers). After a failure, use `pgconn_error_message()` to get a descriptive error string.

```c
if (!pgconn_execute(conn, "INVALID SQL", NULL)) {
    fprintf(stderr, "Query failed: %s\n", pgconn_error_message(conn));
}
```

## Thread Safety

-   **Default functions** (e.g., `pgconn_query`): **Not thread-safe**. The caller must guarantee exclusive access. Ideal for single-threaded or per-thread connection models.
-   **Safe functions** (e.g., `pgconn_query_safe`): **Thread-safe**. They can only be used on a `pgconn_t` object created with `thread_safe = true`.
-   **Manual locking**: `pgconn_lock()` and `pgconn_unlock()` provide fine-grained control for performance-critical sections.

## Requirements

-   A C11-compatible compiler (e.g., GCC, Clang)
-   PostgreSQL `libpq` development libraries and headers
-   POSIX threads (`pthread`) for the thread-safe features

## License

MIT
