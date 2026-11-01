#define _GNU_SOURCE
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <stdarg.h>
#include <sched.h>
#include <time.h>

#define RING_CAP (256 * 1024)   // ~1.5 s de PCM 44.1kHz estéreo 16 bit 
#define CHUNK    4096

/* ------------------------------------------------------------------ *
 *   Orden de locks:  pl.lock  ->  p->m  |  rb.m      (nunca al revés)
 *   - Todo cambio de pl.cur o de la estructura de la lista: wrlock(pl).
 *   - Todo cambio de epoch (rb_flush) ocurre bajo wrlock(pl).
 *   - El productor lee pista actual + epoch bajo rdlock(pl) (atómico).
 * ------------------------------------------------------------------ */

static void set_state(Player *p, PlayState st) {
    pthread_mutex_lock(&p->m);
    p->state = st;
    p->mon_dirty = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_cond_broadcast(&p->mon_cv);
    pthread_mutex_unlock(&p->m);
}

static PlayState get_state(Player *p) {
    pthread_mutex_lock(&p->m);
    PlayState st = p->state;
    pthread_mutex_unlock(&p->m);
    return st;
}

static void request_quit(Player *p) {
    pthread_mutex_lock(&p->m);
    p->quit = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_cond_broadcast(&p->mon_cv);
    pthread_mutex_unlock(&p->m);
    rb_close(&p->rb);
    uint64_t one = 1;
    ssize_t r = write(p->evfd, &one, sizeof one); (void)r;
}

void player_quit(Player *p) { request_quit(p); }

void player_event(Player *p, const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&p->m);
    va_start(ap, fmt);
    vsnprintf(p->last_event, sizeof p->last_event, fmt, ap);
    va_end(ap);
    p->mon_dirty = 1;
    pthread_cond_broadcast(&p->mon_cv);
    pthread_mutex_unlock(&p->m);
}

//procesos auxiliares
static pid_t spawn_sink(int *wfd) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
#ifdef F_SETPIPE_SZ
    fcntl(fds[1], F_SETPIPE_SZ, 4096);// tenemos un pipe pequeño para que pause/skip sean casi inmediatos
#endif
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        dup2(fds[0], STDIN_FILENO);       /* dup2 limpia O_CLOEXEC en el destino */
        /* raw s16le: por defecto 44100 Hz, 2 canales (coincide con el decodificador) */
        execlp("ffplay", "ffplay", "-v", "error", "-nodisp", "-nostats",
               "-f", "s16le", "-i", "pipe:0", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    if (pid < 0) { close(fds[1]); return -1; }
    *wfd = fds[1];
    return pid;
}

static pid_t spawn_decoder(const char *path, int *rfd) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, STDIN_FILENO); dup2(nul, STDERR_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        execlp("ffmpeg", "ffmpeg", "-v", "quiet", "-i", path,
               "-f", "s16le", "-ar", "44100", "-ac", "2", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    *rfd = fds[0];
    return pid;
}

//Hilo productor (Decodifica la pista actual)
static void *producer_thread(void *arg) {
    Player *p = arg;
    unsigned char buf[CHUNK];

    for (;;) {
        atomic_store(&p->prod_cpu, sched_getcpu());
        /* 1. Dormir mientras estemos STOPPED (sin espera activa) */
        pthread_mutex_lock(&p->m);
        while (!p->quit && p->state == ST_STOPPED)
            pthread_cond_wait(&p->cv, &p->m);
        int quit = p->quit;
        pthread_mutex_unlock(&p->m);
        if (quit) break;

        //2.Leer pista actual + epoch de forma atómica (rdlock)
        char *path = NULL; unsigned g = 0; int fin_lista = 0, stopped;
        pthread_rwlock_rdlock(&p->pl.lock);
        pthread_mutex_lock(&p->m);
        stopped = (p->state == ST_STOPPED);
        if (!stopped && !p->pl.cur) { p->state = ST_STOPPED; stopped = 1; fin_lista = 1; }
        pthread_mutex_unlock(&p->m);
        if (!stopped) {
            path = strdup(p->pl.cur->path);
            g = rb_epoch(&p->rb);
            if (!path) { set_state(p, ST_STOPPED); stopped = 1; }
        }
        pthread_rwlock_unlock(&p->pl.lock);
        if (stopped) {
            if (fin_lista) printf("\n[fin de la playlist]\n");
            continue;
        }

    //3. Decodificar y producir hacia el búfer circular
        int fd, aborted = 0;
        pid_t dp = spawn_decoder(path, &fd);
        if (dp < 0) {
            fprintf(stderr, "No se pudo lanzar ffmpeg para %s\n", path);
        } else {
            for (;;) {
                atomic_store(&p->prod_cpu, sched_getcpu());
                ssize_t n = read(fd, buf, sizeof buf);
                if (n < 0) { if (errno == EINTR) continue; break; }
                if (n == 0) break;
                if (rb_put(&p->rb, buf, (size_t)n, g) < 0) { aborted = 1; break; }
            }
            close(fd);
            if (aborted) kill(dp, SIGTERM);
            waitpid(dp, NULL, 0);
        }
        free(path);
        if (aborted) continue;//skip/stop/remove/quit: volver al paso 1

        //Fin natural de pista: avanzar cursor (no se vacía el búfer)
        pthread_rwlock_wrlock(&p->pl.lock);
        if (rb_epoch(&p->rb) == g && p->pl.cur)
            p->pl.cur = p->pl.cur->next;      /* NULL si era la última */
        pthread_rwlock_unlock(&p->pl.lock);
    }
    return NULL;
}

//HIlo consumidor (bufer->ffplay
static void *consumer_thread(void *arg) {
    Player *p = arg;
    unsigned char buf[CHUNK];
    ssize_t n;
    for (;;) {
        atomic_store(&p->cons_cpu, sched_getcpu());
        /* Métrica: ¿nos vamos a bloquear por falta de datos mientras debería sonar? */
        if (rb_count(&p->rb) == 0 && get_state(p) == ST_PLAYING)
            atomic_fetch_add(&p->starves, 1);
        n = rb_get(&p->rb, buf, sizeof buf);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(p->sink_fd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "\nLa salida de audio se cerró.\n");
                request_quit(p);
                return NULL;
            }
            off += w;
        }
    }
    return NULL;
}


int player_init(Player *p) {
    memset(p, 0, sizeof *p);
    p->state = ST_STOPPED;
    if (pl_init(&p->pl) || rb_init(&p->rb, RING_CAP)) return -1;
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   /* para timedwait del monitor */
    pthread_cond_init(&p->mon_cv, &ca);
    pthread_condattr_destroy(&ca);
    p->evfd = eventfd(0, EFD_CLOEXEC);
    if (p->evfd < 0) return -1;
    p->sink_pid = spawn_sink(&p->sink_fd);
    if (p->sink_pid < 0) { perror("spawn ffplay"); return -1; }
    if (pthread_create(&p->th_prod, NULL, producer_thread, p) ||
        pthread_create(&p->th_cons, NULL, consumer_thread, p)) return -1;
    return 0;
}

void player_shutdown(Player *p) {
    request_quit(p);
    kill(p->sink_pid, SIGTERM);              /* desbloquea un write() pendiente */
    pthread_join(p->th_prod, NULL);
    pthread_join(p->th_cons, NULL);
    close(p->sink_fd);
    waitpid(p->sink_pid, NULL, 0);
    pl_destroy(&p->pl);
    rb_destroy(&p->rb);
    pthread_mutex_destroy(&p->m);
    pthread_cond_destroy(&p->cv);
    pthread_cond_destroy(&p->mon_cv);
    close(p->evfd);
}

int player_event_fd(Player *p) { return p->evfd; }

/* ---------------- comandos (escritores de la playlist) ---------------- */
void player_add(Player *p, const char *path) {
    pthread_rwlock_wrlock(&p->pl.lock);
    Track *t = pl_add(&p->pl, path);
    if (t && !p->pl.cur) p->pl.cur = t;
    int n = p->pl.count;
    pthread_rwlock_unlock(&p->pl.lock);
    if (t) { printf("[+] #%d %s\n", n, path); player_event(p, "add #%d", n); }
    else fprintf(stderr, "sin memoria\n");
}

void player_remove(Player *p, int n) {
    pthread_rwlock_wrlock(&p->pl.lock);
    Track *t = pl_nth(&p->pl, n);
    if (!t) { pthread_rwlock_unlock(&p->pl.lock); printf("No existe la pista #%d\n", n); return; }
    if (t == p->pl.cur) {                    /* se elimina la pista sonando: saltar a la siguiente */
        p->pl.cur = t->next;
        rb_flush(&p->rb);
    }
    pl_unlink(&p->pl, t);
    free(t->path); free(t);
    pthread_rwlock_unlock(&p->pl.lock);
    printf("[-] pista #%d eliminada\n", n);
    player_event(p, "rm #%d", n);
}

void player_move(Player *p, int from, int to) {
    pthread_rwlock_wrlock(&p->pl.lock);
    Track *t = pl_nth(&p->pl, from);
    if (!t) { pthread_rwlock_unlock(&p->pl.lock); printf("No existe la pista #%d\n", from); return; }
    pl_unlink(&p->pl, t);                    /* cur apunta al nodo, no a una posición */
    pl_insert_at(&p->pl, t, to);
    pthread_rwlock_unlock(&p->pl.lock);
    printf("[~] pista #%d movida a la posición %d\n", from, to);
    player_event(p, "mv #%d -> #%d", from, to);
}

void player_clear(Player *p) {
    pthread_rwlock_wrlock(&p->pl.lock);
    pl_clear(&p->pl);
    set_state(p, ST_STOPPED);
    rb_flush(&p->rb);
    rb_set_paused(&p->rb, 0);
    pthread_rwlock_unlock(&p->pl.lock);
    printf("[x] playlist vaciada\n");
    player_event(p, "clear");
}

void player_list(Player *p) {
    pthread_rwlock_rdlock(&p->pl.lock);
    if (!p->pl.count) printf("(playlist vacía)\n");
    int i = 1;
    for (Track *t = p->pl.head; t; t = t->next, i++)
        printf("%s %2d. %s\n", t == p->pl.cur ? "▶" : " ", i, t->path);
    pthread_rwlock_unlock(&p->pl.lock);
}

/* ---------------- control de reproducción ---------------- */
void player_play(Player *p) {
    pthread_rwlock_wrlock(&p->pl.lock);
    PlayState st = get_state(p);
    if (st == ST_PAUSED) {
        set_state(p, ST_PLAYING);
        rb_set_paused(&p->rb, 0);
    } else if (st == ST_STOPPED) {
        if (!p->pl.cur) p->pl.cur = p->pl.head;
        if (!p->pl.cur) printf("La playlist está vacía.\n");
        else {
            rb_flush(&p->rb);
            rb_set_paused(&p->rb, 0);
            set_state(p, ST_PLAYING);
        }
    }
    pthread_rwlock_unlock(&p->pl.lock);
    player_event(p, "play");
}

void player_pause(Player *p) {
    if (get_state(p) != ST_PLAYING) return;
    set_state(p, ST_PAUSED);
    rb_set_paused(&p->rb, 1);                /* el consumidor se duerme; el productor sigue llenando */
    player_event(p, "pause");
}

void player_stop(Player *p) {
    pthread_rwlock_wrlock(&p->pl.lock);
    set_state(p, ST_STOPPED);
    rb_flush(&p->rb);
    rb_set_paused(&p->rb, 0);
    pthread_rwlock_unlock(&p->pl.lock);
    player_event(p, "stop");
}

void player_next(Player *p) {
    pthread_rwlock_wrlock(&p->pl.lock);
    if (!p->pl.cur) p->pl.cur = p->pl.head;
    else if (p->pl.cur->next) p->pl.cur = p->pl.cur->next;
    else { p->pl.cur = NULL; printf("[fin de la playlist]\n"); set_state(p, ST_STOPPED); }
    rb_flush(&p->rb);
    pthread_rwlock_unlock(&p->pl.lock);
    player_event(p, "next");
}

void player_prev(Player *p) {
    pthread_rwlock_wrlock(&p->pl.lock);
    if (!p->pl.cur) p->pl.cur = p->pl.tail;
    else if (p->pl.cur->prev) p->pl.cur = p->pl.cur->prev;   /* si es la primera, reinicia */
    rb_flush(&p->rb);
    pthread_rwlock_unlock(&p->pl.lock);
    player_event(p, "prev");
}

void player_status(Player *p) {
    static const char *names[] = {"DETENIDO", "REPRODUCIENDO", "PAUSADO"};
    pthread_rwlock_rdlock(&p->pl.lock);
    PlayState st = get_state(p);
    printf("Estado: %s | Pista: %s\n", names[st], p->pl.cur ? p->pl.cur->path : "(ninguna)");
    pthread_rwlock_unlock(&p->pl.lock);
    pthread_mutex_lock(&p->rb.m);
    printf("Búfer: %zu/%zu bytes (%.0f%%)\n", p->rb.count, p->rb.cap, 100.0 * p->rb.count / p->rb.cap);
    pthread_mutex_unlock(&p->rb.m);
}
