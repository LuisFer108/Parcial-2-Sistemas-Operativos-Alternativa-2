#ifndef RINGBUF_H
#define RINGBUF_H
#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

/* Búfer circular de bytes para el patrón Productor-Consumidor.
 * Un único mutex protege TODOS los campos; dos variables de condición
 * evitan espera activa:
 *   not_full  : el productor duerme aquí cuando el búfer está lleno
 *   not_empty : el consumidor duerme aquí cuando está vacío o en pausa
 * 'epoch' numera la "generación" del contenido: cada skip/stop/remove la
 * incrementa y vacía el búfer, invalidando lo que el productor aún traiga. */
typedef struct {
    unsigned char *data;
    size_t cap, head, tail, count;   //head = lectura, tail = escritura
    unsigned epoch;
    unsigned long long total_in, total_out;   //bytes producidos / consumidos (métricas) 
    int paused, closed;
    pthread_mutex_t m;
    pthread_cond_t not_full, not_empty;
} RingBuf;

int     rb_init(RingBuf *rb, size_t cap);
void    rb_destroy(RingBuf *rb);
int     rb_put(RingBuf *rb, const unsigned char *src, size_t len, unsigned epoch); //0 ok, -1 epoch cambió/cerrado 
ssize_t rb_get(RingBuf *rb, unsigned char *dst, size_t max);                        //>0 bytes, 0 cerrado
void    rb_flush(RingBuf *rb);          // epoch++, vacía y despierta a todos
void    rb_set_paused(RingBuf *rb, int paused);
unsigned rb_epoch(RingBuf *rb);
void    rb_close(RingBuf *rb);
size_t  rb_count(RingBuf *rb);
#endif
