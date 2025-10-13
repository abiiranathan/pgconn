# PostgreSQL Connection Pool (libpgpool)

A lightweight, thread-safe connection pool for PostgreSQL implemented in C using libpq.

## Features

- **Thread-safe** connection pooling
- **Configurable** pool size (min/max connections)
- **Connection timeouts** for both acquisition and queries
- **Prepared statement** support
- **Parameterized queries** via `pgpool_query_params()` (no explicit prepare needed)
- **Transaction management** (begin/commit/rollback)
- **Automatic reconnection** for broken connections
- **Customizable** with connection init/close callbacks
- **Performance metrics** (active/idle connections)
 - **CMake and pkg-config** support for building and consumption

## Installation

### From Source (CMake)

```bash
git clone https://github.com/abiiranathan/libpgpool.git
cd libpgpool
cmake -S . -B build
cmake --build build -j
sudo cmake --install build
```

By default this installs:
- Library to `/usr/local/lib`
- Headers to `/usr/local/include/pgpool`
- CMake package config to `/usr/local/lib/cmake/pgpool`
- pkg-config file to `/usr/local/lib/pkgconfig/pgpool.pc`

You can customize the install prefix with `-DCMAKE_INSTALL_PREFIX=/opt/pgpool`.

### From Source (Makefile)

The provided Makefile is a thin driver around CMake to keep configuration in your build process.

Basic build/install:
```bash
git clone https://github.com/abiiranathan/libpgpool.git
cd libpgpool
make                 # defaults to BUILD_TYPE=Release
sudo make install    # installs to /usr/local by default
```

Common targets:
```bash
make debug           # BUILD_TYPE=Debug
make release         # BUILD_TYPE=Release
make relwithdebinfo  # BUILD_TYPE=RelWithDebInfo
make sanitize        # Debug with AddressSanitizer
```

Useful variables (override on the command line):
```bash
CC=clang                         # choose compiler
BUILD_DIR=out                    # build directory (default: build)
BUILD_TYPE=Release               # CMake build type
INSTALL_PREFIX=/opt/pgpool       # CMAKE_INSTALL_PREFIX
CFLAGS="-Wall -Wextra -Wpedantic -Werror"  # passed to CMake C flags
LDFLAGS="-Wl,--as-needed"       # passed to shared/exe linker flags
GENERATOR="Ninja"               # optional CMake generator
```

Example:
```bash
make release CC=clang INSTALL_PREFIX=/opt/pgpool CFLAGS="-O3 -DNDEBUG -march=native"
sudo make install INSTALL_PREFIX=/opt/pgpool
```

### Linking with Your Application

Using pkg-config:
```
cc app.c $(pkg-config --cflags --libs pgpool)
```

Using CMake (consumer project):
```cmake
find_package(pgpool CONFIG REQUIRED)
add_executable(app app.c)
target_link_libraries(app PRIVATE pgpool::pgpool)
```

Manual flags (if not using pkg-config/CMake):
```
-I/usr/local/include/pgpool -L/usr/local/lib -lpgpool -lpq -pthread
```

## Usage

### Basic Example

```c
#include <pgpool.h>

int main() {
    pgpool_config_t config = {
        .conninfo = "postgresql://user:password@localhost/dbname",
        .min_connections = 2,
        .max_connections = 10,
        .connect_timeout = 5,
        .auto_reconnect = true
    };

    pgpool_t* pool = pgpool_create(&config);
    if (!pool) return 1;

    pgconn_t* conn = pgpool_acquire(pool, 1000); // 1 second timeout
    if (!conn) {
        pgpool_destroy(pool);
        return 1;
    }

    // Execute a query
    PGresult* res = pgpool_query(conn, "SELECT version()", 1000);
    if (res) {
        printf("%s\n", PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    pgpool_release(pool, conn);
    pgpool_destroy(pool);
    return 0;
}
```

### Prepared Statement Example

```c
// Prepare statement
if (pgpool_prepare(conn, "get_user", "SELECT * FROM users WHERE id = $1", 1, NULL, 1000)) {
    const char* params[] = {"1"};

    res = pgpool_execute_prepared(conn, "get_user", 1, params, NULL, NULL, 0, 1000);
    if (res) {
        // Process results...
        PQclear(res);
    }
    pgpool_deallocate(conn, "get_user", 1000);
}
```

### Parameterized Query Example (no explicit prepare)

```c
const char* vals[] = {"1"};
PGresult* res = pgpool_query_params(
    conn,
    "SELECT * FROM users WHERE id = $1",
    1,               // n_params
    NULL,            // param_types (let server infer)
    vals,            // param_values
    NULL,            // param_lengths (text)
    NULL,            // param_formats (all text)
    0,               // result_format: 0=text, 1=binary
    1000             // timeout_ms
);
if (res) {
    // Process results...
    PQclear(res);
} else {
    fprintf(stderr, "error: %s\n", pgpool_error_message(conn));
}
```

## API Documentation

### Core Functions

| Function           | Description                           |
| ------------------ | ------------------------------------- |
| `pgpool_create()`  | Create a new connection pool          |
| `pgpool_destroy()` | Destroy a connection pool             |
| `pgpool_acquire()` | Acquire a connection from the pool    |
| `pgpool_release()` | Release a connection back to the pool |

### Query Execution

| Function                    | Description                        |
| --------------------------- | ---------------------------------- |
| `pgpool_query()`            | Execute a query and return results |
| `pgpool_query_params()`     | Execute a parameterized query      |
| `pgpool_execute()`          | Execute a query without results    |
| `pgpool_prepare()`          | Create a prepared statement        |
| `pgpool_execute_prepared()` | Execute a prepared statement       |
| `pgpool_deallocate()`       | Deallocate a prepared statement    |

### Transaction Management

| Function            | Description            |
| ------------------- | ---------------------- |
| `pgpool_begin()`    | Start a transaction    |
| `pgpool_commit()`   | Commit a transaction   |
| `pgpool_rollback()` | Rollback a transaction |

## Configuration

The `pgpool_config_t` structure allows you to configure:

```c
typedef struct {
    const char* conninfo;          // PostgreSQL connection string
    size_t min_connections;        // Minimum connections (default: 1)
    size_t max_connections;        // Maximum connections (default: 10)
    int connect_timeout;           // Timeout in seconds (default: 5)
    bool auto_reconnect;           // Reconnect automatically (default: true)
    void (*connection_init)(PGconn*);   // Connection init callback
    void (*connection_close)(PGconn*);  // Connection close callback
} pgpool_config_t;
```

## Thread Safety

All public functions are thread-safe. Multiple threads can safely:
- Acquire/release connections
- Execute queries
- Manage transactions


## More utilities
- [pgtypes.h](./pgtypes.h) : Contains value conversion functions.
- [pg_iter.h](./pgiter.h) : Contains iterator helpers around PQResult.

## Building Tests

```bash
make test
./test
```

## License

MIT License - See LICENSE file for details.

## Contributing

Pull requests and issues are welcome! Please ensure:
- Code follows the existing style
- Tests are updated
- Documentation is maintained
