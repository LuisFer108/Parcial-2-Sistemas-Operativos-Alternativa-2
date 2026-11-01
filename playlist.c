#include "playlist.h"
#include <stdlib.h>
#include <string.h>

int pl_init(Playlist *pl) {
    pl->head = pl->tail = pl->cur = NULL;
    pl->count = 0;
    return pthread_rwlock_init(&pl->lock, NULL);
}

void pl_destroy(Playlist *pl) {
    pl_clear(pl);
    pthread_rwlock_destroy(&pl->lock);
}

Track *pl_add(Playlist *pl, const char *path) {
    Track *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->path = strdup(path);
    if (!t->path) { free(t); return NULL; }
    t->prev = pl->tail;
    if (pl->tail) pl->tail->next = t; else pl->head = t;
    pl->tail = t;
    pl->count++;
    return t;
}

Track *pl_nth(Playlist *pl, int n) {
    if (n < 1 || n > pl->count) return NULL;
    Track *t = pl->head;
    while (--n > 0) t = t->next;
    return t;
}

void pl_unlink(Playlist *pl, Track *t) {
    if (t->prev) t->prev->next = t->next; else pl->head = t->next;
    if (t->next) t->next->prev = t->prev; else pl->tail = t->prev;
    t->prev = t->next = NULL;
    pl->count--;
}

void pl_insert_at(Playlist *pl, Track *t, int pos) {
    if (pos < 1) pos = 1;
    Track *at = (pos <= pl->count) ? pl_nth(pl, pos) : NULL;   //insertar antes de 'at'
    if (!at) {
        t->prev = pl->tail; t->next = NULL;
        if (pl->tail) pl->tail->next = t; else pl->head = t;
        pl->tail = t;
    } else {
        t->next = at; t->prev = at->prev;
        if (at->prev) at->prev->next = t; else pl->head = t;
        at->prev = t;
    }
    pl->count++;
}

void pl_clear(Playlist *pl) {
    Track *t = pl->head;
    while (t) { Track *n = t->next; free(t->path); free(t); t = n; }
    pl->head = pl->tail = pl->cur = NULL;
    pl->count = 0;
}
