#ifndef EPASS_GAME_SPSC_QUEUE_H
#define EPASS_GAME_SPSC_QUEUE_H

#include <pthread.h>
#include <stddef.h>

typedef struct {
    void **buf;
    size_t capacity, head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty, not_full;
    int closed;
} spsc_bq_t;

int spsc_bq_init(spsc_bq_t *q, size_t capacity);
void spsc_bq_destroy(spsc_bq_t *q);
int spsc_bq_push(spsc_bq_t *q, void *item);
int spsc_bq_pop(spsc_bq_t *q, void **out);
int spsc_bq_try_push(spsc_bq_t *q, void *item);
int spsc_bq_try_pop(spsc_bq_t *q, void **out);
void spsc_bq_close(spsc_bq_t *q);

#endif
