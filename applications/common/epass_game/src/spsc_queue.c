#include "spsc_queue.h"

#include <errno.h>
#include <stdlib.h>

int spsc_bq_init(spsc_bq_t *q, size_t capacity)
{
    int ret;
    if(!q || !capacity) return EINVAL;
    q->buf = malloc(sizeof(*q->buf) * capacity);
    if(!q->buf) return ENOMEM;
    q->capacity = capacity;
    q->head = q->tail = q->count = 0;
    q->closed = 0;
    if((ret = pthread_mutex_init(&q->mtx, NULL))) goto fail_buf;
    if((ret = pthread_cond_init(&q->not_empty, NULL))) goto fail_mutex;
    if((ret = pthread_cond_init(&q->not_full, NULL))) goto fail_empty;
    return 0;
fail_empty:
    pthread_cond_destroy(&q->not_empty);
fail_mutex:
    pthread_mutex_destroy(&q->mtx);
fail_buf:
    free(q->buf);
    q->buf = NULL;
    return ret;
}

void spsc_bq_close(spsc_bq_t *q)
{
    if(!q || !q->buf) return;
    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

void spsc_bq_destroy(spsc_bq_t *q)
{
    if(!q || !q->buf) return;
    spsc_bq_close(q);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->mtx);
    free(q->buf);
    q->buf = NULL;
}

int spsc_bq_push(spsc_bq_t *q, void *item)
{
    int ret = 0;
    if(!q) return EINVAL;
    pthread_mutex_lock(&q->mtx);
    while(q->count == q->capacity && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mtx);
    if(q->closed) ret = EPIPE;
    else {
        q->buf[q->tail] = item;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mtx);
    return ret;
}

int spsc_bq_pop(spsc_bq_t *q, void **out)
{
    int ret = 0;
    if(!q || !out) return EINVAL;
    pthread_mutex_lock(&q->mtx);
    while(!q->count && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mtx);
    if(!q->count) ret = EPIPE;
    else {
        *out = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mtx);
    return ret;
}

int spsc_bq_try_push(spsc_bq_t *q, void *item)
{
    int ret = 0;
    if(!q) return EINVAL;
    pthread_mutex_lock(&q->mtx);
    if(q->closed) ret = EPIPE;
    else if(q->count == q->capacity) ret = EAGAIN;
    else {
        q->buf[q->tail] = item;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mtx);
    return ret;
}

int spsc_bq_try_pop(spsc_bq_t *q, void **out)
{
    int ret = 0;
    if(!q || !out) return EINVAL;
    pthread_mutex_lock(&q->mtx);
    if(!q->count) ret = q->closed ? EPIPE : EAGAIN;
    else {
        *out = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mtx);
    return ret;
}
