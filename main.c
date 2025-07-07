#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "pgpool.h"

#define THREAD_COUNT 4
#define ITERATIONS 5

typedef struct {
    pgpool_t* pool;
    int thread_id;
} thread_data_t;

void* thread_func(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    pgpool_t* pool = data->pool;
    int thread_id = data->thread_id;

    for (int i = 0; i < ITERATIONS; i++) {
        printf("Thread %d, iteration %d\n", thread_id, i);

        // Acquire connection with 1 second timeout
        pgconn_t* conn = pgpool_acquire(pool, 1000);
        if (!conn) {
            fprintf(stderr, "Thread %d failed to acquire connection\n", thread_id);
            continue;
        }

        // Execute a simple query
        PGresult* res = pgpool_query(conn, "SELECT 1", 1000);
        if (res) {
            PQclear(res);
        } else {
            fprintf(stderr, "Thread %d query failed: %s\n", thread_id, pgpool_error_message(conn));
        }

        // Execute a prepared statement
        if (pgpool_prepare(conn, "get_user", "SELECT * FROM users WHERE id = $1", 1, NULL, 1000)) {
            const char* params[] = {"1"};

            res = pgpool_execute_prepared(conn, "get_user", 1, params, NULL, NULL, 0, 1000);
            if (res) {
                PQclear(res);
            } else {
                fprintf(stderr, "Thread %d prepared statement failed: %s\n", thread_id,
                        pgpool_error_message(conn));
            }
            pgpool_deallocate(conn, "get_user", 1000);
        } else {
            fprintf(stderr, "Thread %d prepare failed: %s\n", thread_id, pgpool_error_message(conn));
        }

        // Release connection back to pool
        pgpool_release(pool, conn);

        // Small delay to simulate think time
        usleep(10000 * (1 + (rand() % 5)));
    }

    return NULL;
}

int main() {
    const char* conninfo = getenv("POSTGRES_URI");
    if (!conninfo) {
        fprintf(stderr, "POSTGRES_URI environment variable not set\n");
        return 1;
    }

    pgpool_config_t config = {
        .conninfo = conninfo,
        .min_connections = 2,
        .max_connections = 20,
        .connect_timeout = 3,
        .auto_reconnect = true,
    };

    pgpool_t* pool = pgpool_create(&config);
    if (!pool) {
        return 1;
    }

    pthread_t threads[THREAD_COUNT];
    thread_data_t thread_data[THREAD_COUNT];

    // Create threads
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_data[i].pool = pool;
        thread_data[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, thread_func, &thread_data[i]) != 0) {
            perror("Failed to create thread");
            pgpool_destroy(pool);
            return 1;
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    pgpool_destroy(pool);
    return 0;
}
