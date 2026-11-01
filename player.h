#ifndef PLAYER_H
#define PLAYER_H
#include <pthread.h>
#include <sys/types.h>
#include <stdatomic.h>
#include "playlist.h"
#include "ringbuf.h"

typedef enum { ST_STOPPED, ST_PLAYING, ST_PAUSED } PlayState;

typedef struct {
    Playlist pl;
    RingBuf  rb;
    pthread_mutex_t m;          //protege state y quit
    pthread_cond_t  cv;         //el productor duerme aquí mientras esta en estado stopped
    PlayState state;
    int quit;
    pthread_cond_t mon_cv;      //despierta al monitor (evento o quit)
    int mon_dirty;              //hay algo nuevo que dibujar (protegido por m)
    char last_event[96];        //último evento, para el monitor (protegido por m)
    _Atomic int prod_cpu, cons_cpu; //núcleo donde corre cada hilo */
    _Atomic unsigned starves;   //veces que el consumidor encontró el búfer vacío en estado playing
    int evfd;                   //eventfd: despierta a la CLI sin polling
    pthread_t th_prod, th_cons;
    pid_t sink_pid;             //ffplay (salida de audio)
    int   sink_fd;
} Player;

int  player_init(Player *p);
void player_shutdown(Player *p);
int  player_event_fd(Player *p);
void player_quit(Player *p);              
void player_event(Player *p, const char *fmt, ...);

/* Comandos (llamados desde el hilo de la CLI) */
void player_add(Player *p, const char *path);
void player_remove(Player *p, int n);
void player_move(Player *p, int from, int to);
void player_clear(Player *p);
void player_list(Player *p);
void player_play(Player *p);
void player_pause(Player *p);
void player_stop(Player *p);
void player_next(Player *p);
void player_prev(Player *p);
void player_status(Player *p);
#endif
