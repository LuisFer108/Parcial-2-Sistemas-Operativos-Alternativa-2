#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>

int rb_init(RingBuf *rb, size_t cap) {
    rb->data = malloc(cap);
    if (!rb->data) return -1;
    rb->cap = cap; rb->head = rb->tail = rb->count = 0;
    rb->epoch = 0; rb->paused = 0; rb->closed = 0;
    rb->total_in = rb->total_out = 0;
    pthread_mutex_init(&rb->m, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    return 0;
}

void rb_destroy(RingBuf *rb) {
    pthread_mutex_destroy(&rb->m);
    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->not_empty);
    free(rb->data);
}

int rb_put(RingBuf *rb, const unsigned char *src, size_t len, unsigned epoch) {
    pthread_mutex_lock(&rb->m);
    while (len > 0) {
        while (rb->count == rb->cap && rb->epoch == epoch && !rb->closed)
            pthread_cond_wait(&rb->not_full, &rb->m);
        if (rb->epoch != epoch || rb->closed) {      //contenido obsoleto: abortar 
            pthread_mutex_unlock(&rb->m);
            return -1;
        }
        size_t n = rb->cap - rb->count; if (n > len) n = len;
        size_t first = rb->cap - rb->tail; if (first > n) first = n;
        memcpy(rb->data + rb->tail, src, first);
        memcpy(rb->data, src + first, n - first);
        rb->tail = (rb->tail + n) % rb->cap;
        rb->count += n; rb->total_in += n; src += n; len -= n;
        pthread_cond_signal(&rb->not_empty);
    }
    pthread_mutex_unlock(&rb->m);
    return 0;
}

ssize_t rb_get(RingBuf *rb, unsigned char *dst, size_t max) {
    pthread_mutex_lock(&rb->m);
    while (!rb->closed && (rb->count == 0 || rb->paused))
        pthread_cond_wait(&rb->not_empty, &rb->m);
    if (rb->closed) { pthread_mutex_unlock(&rb->m); return 0; }
    size_t n = rb->count < max ? rb->count : max;
    size_t first = rb->cap - rb->head; if (first > n) first = n;
    memcpy(dst, rb->data + rb->head, first);
    memcpy(dst + first, rb->data, n - first);
    rb->head = (rb->head + n) % rb->cap;
    rb->count -= n; rb->total_out += n;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->m);
    return (ssize_t)n;
}

void rb_flush(RingBuf *rb) {
    pthread_mutex_lock(&rb->m);
    rb->epoch++;
    rb->head = rb->tail = rb->count = 0;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->m);
}

void rb_set_paused(RingBuf *rb, int paused) {
    pthread_mutex_lock(&rb->m);
    rb->paused = paused;
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->m);
}

unsigned rb_epoch(RingBuf *rb) {
    pthread_mutex_lock(&rb->m);
    unsigned e = rb->epoch;
    pthread_mutex_unlock(&rb->m);
    return e;
}

void rb_close(RingBuf *rb) {
    pthread_mutex_lock(&rb->m);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->m);
}

size_t rb_count(RingBuf *rb) {
    pthread_mutex_lock(&rb->m);
    size_t c = rb->count;
    pthread_mutex_unlock(&rb->m);
    return c;
}
