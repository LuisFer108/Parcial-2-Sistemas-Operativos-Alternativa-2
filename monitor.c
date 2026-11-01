#define _GNU_SOURCE
#include "monitor.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------
 * HILO MONITOR (solo lectura): nunca modifica el estado del reproductor.
 * Se despierta (a) por evento: set_state()/player_event() señalizan mon_cv
 * y (b) por temporizador: pthread_cond_timedwait cada REFRESH_MS para
 * refrescar tasas y reloj. No hay bucle de sondeo ni sleep().
 * Orden de locks: toma pl.lock(rd), p->m y rb.m SECUENCIALMENTE (nunca
 * anidados), respetando el protocolo del resto del programa.
 * ------------------------------------------------------------------ */

#define REFRESH_MS 250
#define BYTES_PER_SEC (44100 * 2 * 2)     //PCM s16le estéreo 44.1 kHz 
#define HEADER_LINES 4                    //3 de panel + 1 separador 

static pthread_t th;
static int running = 0, rows = 24;
static Player *P;

static void fmt_bar(char *out, size_t sz, double frac, int width) {
    int filled = (int)(frac * width + 0.5);
    out[0] = 0;
    for (int i = 0; i < width; i++)
        strncat(out, i < filled ? "█" : "░", sz - strlen(out) - 1);
}

static void render(double in_rate, double out_rate) {
    static const char *names[]  = {"DETENIDO", "REPRODUCIENDO", "PAUSADO"};
    static const char *icons[]  = {"■", "▶", "⏸"};
    static const char *colors[] = {"31", "32", "33"};

    //instantánea de la playlist (rdlock breve)
    char title[64] = "(ninguna)"; int idx = 0, total;
    pthread_rwlock_rdlock(&P->pl.lock);
    total = P->pl.count;
    if (P->pl.cur) {
        const char *b = strrchr(P->pl.cur->path, '/');
        snprintf(title, sizeof title, "%.40s", b ? b + 1 : P->pl.cur->path);
        for (Track *t = P->pl.head; t; t = t->next) { idx++; if (t == P->pl.cur) break; }
    }
    pthread_rwlock_unlock(&P->pl.lock);

    //estado y último evento
    char ev[96];
    pthread_mutex_lock(&P->m);
    PlayState st = P->state;
    snprintf(ev, sizeof ev, "%s", P->last_event[0] ? P->last_event : "-");
    pthread_mutex_unlock(&P->m);

    //búfer
    pthread_mutex_lock(&P->rb.m);
    size_t cnt = P->rb.count, cap = P->rb.cap;
    unsigned long long tout = P->rb.total_out;
    unsigned epoch = P->rb.epoch;
    pthread_mutex_unlock(&P->rb.m);

    //actividad de hilos (derivada del estado; no requiere más sincronización)
    const char *prod = st == ST_STOPPED ? "dormido"
                     : cnt == cap       ? "bloqueado (búfer lleno)"
                                        : "decodificando";
    const char *cons = st == ST_PAUSED  ? "en pausa"
                     : cnt == 0 && st == ST_PLAYING ? "ESPERANDO DATOS"
                     : cnt == 0         ? "inactivo"
                                        : "enviando a ffplay";

    char bar[128];
    double frac = cap ? (double)cnt / (double)cap : 0.0;
    fmt_bar(bar, sizeof bar, frac, 20);
    unsigned long long secs = tout / BYTES_PER_SEC;

    //Un solo printf bajo el lock de stdio => atómico respecto a los mensajes de la CLI 
    printf("\0337\033[?7l"                                   //guardar cursor, sin autowrap 
           "\033[1;1H\033[K \033[1;%sm%s %s\033[0m  Pista %d/%d  \033[1m%s\033[0m   ⏱ %02llu:%02llu"
           "\033[2;1H\033[K Búfer [%s] %3.0f%%  %zu/%zu KB   ↓ prod %.0f KB/s   ↑ cons %.0f KB/s   vacíos: %u   epoch: %u"
           "\033[3;1H\033[K Productor: %s [CPU %d]   Consumidor: %s [CPU %d]   Evento: %s"
           "\033[?7h\0338",
           colors[st], icons[st], names[st], idx, total, title, secs / 60, secs % 60,
           bar, frac * 100.0, cnt / 1024, cap / 1024, in_rate / 1024.0, out_rate / 1024.0,
           atomic_load(&P->starves), epoch,
           prod, atomic_load(&P->prod_cpu), cons, atomic_load(&P->cons_cpu), ev);
    fflush(stdout);
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *monitor_thread(void *arg) {
    (void)arg;
    unsigned long long prev_in = 0, prev_out = 0;
    double prev_t = now_s();
    double in_rate = 0, out_rate = 0;

    for (;;) {
        //Dormir hasta: evento (mon_dirty), timeout de refresco, o quit
        struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_nsec += REFRESH_MS * 1000000L;
        dl.tv_sec  += dl.tv_nsec / 1000000000L; dl.tv_nsec %= 1000000000L;

        pthread_mutex_lock(&P->m);
        while (!P->quit && !P->mon_dirty) {
            if (pthread_cond_timedwait(&P->mon_cv, &P->m, &dl) == ETIMEDOUT) break;
        }
        int quit = P->quit;
        P->mon_dirty = 0;
        pthread_mutex_unlock(&P->m);
        if (quit) break;

        //Tasas (bytes/s) calculadas con contadores del búfer
        pthread_mutex_lock(&P->rb.m);
        unsigned long long tin = P->rb.total_in, tout = P->rb.total_out;
        pthread_mutex_unlock(&P->rb.m);
        double t = now_s();
        if (t - prev_t >= 0.2) {
            in_rate  = (double)(tin  - prev_in)  / (t - prev_t);
            out_rate = (double)(tout - prev_out) / (t - prev_t);
            prev_in = tin; prev_out = tout; prev_t = t;
        }
        render(in_rate, out_rate);
    }
    return NULL;
}

int monitor_start(Player *p) {
    struct winsize ws;
    if (!isatty(STDOUT_FILENO) || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_row < 10)
        return -1;
    P = p; rows = ws.ws_row;
    /* Limpiar, dibujar separador y fijar la región de scroll debajo del panel */
    printf("\033[2J\033[%d;1H\033[2m", HEADER_LINES);
    for (int i = 0; i < ws.ws_col; i++) fputs("─", stdout);
    printf("\033[0m\033[%d;%dr\033[%d;1H", HEADER_LINES + 1, rows, HEADER_LINES + 1);
    fflush(stdout);
    running = 1;
    if (pthread_create(&th, NULL, monitor_thread, NULL)) { running = 0; return -1; }
    return 0;
}

void monitor_stop(void) {
    if (!running) return;
    pthread_join(th, NULL);                 //el hilo termina porque player_quit() ya despertó mon_cv 
    printf("\033[r\033[%d;1H\n", rows);     //restaurar región de scroll completa 
    fflush(stdout);
    running = 0;
}
