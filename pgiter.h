#ifndef PG_ITER_H
#define PG_ITER_H

#include <libpq-fe.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PGresult* result;
    size_t current_row;
    size_t total_rows;
    int num_fields;
} pg_iterator_t;

// Create a new iterator for a PGresult.
// The iterator will iterate over the rows of the result set.
// The result must not be NULL and should be valid until the iterator is used.
static inline pg_iterator_t pg_iter_create(PGresult* result) {
    return (pg_iterator_t){
        .result      = result,
        .current_row = 0,
        .total_rows  = result ? (size_t)PQntuples(result) : 0,
        .num_fields  = result ? PQnfields(result) : 0,
    };
}

// Returns true if there are more rows to iterate over.
// Advances the iterator to the next row.
static inline bool pg_iter_next(pg_iterator_t* iter) {
    return iter->current_row < iter->total_rows;
}

// Get the value of a specific field in the current row.
// Returns a pointer to the string data (valid until PQclear is called).
// If the current row is out of bounds, returns NULL.
static inline const char* pg_iter_get(pg_iterator_t* iter, int field) {
    if (iter->current_row >= iter->total_rows) return NULL;
    return PQgetvalue(iter->result, iter->current_row++, field);
}

#ifdef __cplusplus
}
#endif

#endif  // PG_ITER_H
