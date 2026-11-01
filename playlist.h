#ifndef PLAYLIST_H
#define PLAYLIST_H
#include <pthread.h>

/* Lista doblemente enlazada. El rwlock vive aquí, pero el PROTOCOLO de uso (quién toma rdlock y quién wrlock) lo aplica player.c.
 * Todas las funciones pl_* exigen que el llamador ya tenga el lock adecuado. */
typedef struct Track {
    char *path;
    struct Track *prev, *next;
} Track;

typedef struct {
    Track *head, *tail;
    Track *cur;                 //pista en reproducción (NULL = ninguna/fin)
    int count;
    pthread_rwlock_t lock;
} Playlist;

int    pl_init(Playlist *pl);
void   pl_destroy(Playlist *pl);
Track *pl_add(Playlist *pl, const char *path);   //al final
Track *pl_nth(Playlist *pl, int n);              //n es 1-based
void   pl_unlink(Playlist *pl, Track *t);        //no libera, no toca cur
void   pl_insert_at(Playlist *pl, Track *t, int pos);
void   pl_clear(Playlist *pl);
#endif
