#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "pgconn.h"

#define THREAD_COUNT 4
#define ITERATIONS   5

typedef struct {
    pgconn_t* conn;
    int thread_id;
} thread_data_t;

void* thread_func(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    pgconn_t* conn      = data->conn;
    int thread_id       = data->thread_id;

    for (int i = 0; i < ITERATIONS; i++) {
        printf("Thread %d, iteration %d\n", thread_id, i);

        // Execute a simple query
        PGresult* res = pgconn_query_safe(conn, "SELECT 1", NULL);
        if (res) {
            PQclear(res);
        } else {
            fprintf(stderr, "Thread %d query failed: %s\n", thread_id, pgconn_error_message_safe(conn));
        }

        // Execute a prepared statement
        if (pgconn_prepare_safe(conn, "get_user", "SELECT * FROM users WHERE id = $1", 1, NULL)) {
            const char* params[] = {"1"};
            res                  = pgconn_execute_prepared_safe(conn, "get_user", 1, params, NULL);
            if (res) {
                PQclear(res);
            } else {
                const char* fmt = "Thread %d prepared statement failed: %s\n";
                fprintf(stderr, fmt, thread_id, pgconn_error_message_safe(conn));
            }
            pgconn_deallocate_safe(conn, "get_user");
        } else {
            fprintf(stderr, "Thread %d prepare failed: %s\n", thread_id, pgconn_error_message_safe(conn));
        }

        // Small delay to simulate think time
        usleep((unsigned)(10000 * (1 + (rand() % 5))));
    }

    return NULL;
}

int main() {
    const char* conninfo = getenv("POSTGRES_URI");
    if (!conninfo) {
        fprintf(stderr, "POSTGRES_URI environment variable not set\n");
        return 1;
    }

    pgconn_config_t config = {
        .conninfo       = conninfo,
        .auto_reconnect = true,
        .thread_safe    = true,
    };

    pgconn_t* conn = pgconn_create(&config);
    if (!conn) {
        return 1;
    }

    pthread_t threads[THREAD_COUNT];
    thread_data_t thread_data[THREAD_COUNT];

    // Create threads
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_data[i].conn      = conn;
        thread_data[i].thread_id = i;

        if (pthread_create(&threads[i], NULL, thread_func, &thread_data[i]) != 0) {
            perror("Failed to create thread");
            pgconn_destroy(conn);
            return 1;
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    pgconn_destroy(conn);
    return 0;
}
