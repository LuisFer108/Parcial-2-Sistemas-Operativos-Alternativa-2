#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include "player.h"
#include "monitor.h"

/* Hilo principal = CLI. Duerme en poll() sobre {stdin, eventfd}: cero espera activa. */

static volatile int g_evfd = -1;
static void on_signal(int s) { (void)s; uint64_t v = 1; ssize_t r = write(g_evfd, &v, sizeof v); (void)r; }

static void ayuda(void) {
    puts("Comandos:\n"
         "  add <ruta>     agrega una pista        rm <n>        elimina la pista n\n"
         "  mv <a> <b>     mueve la pista a -> b   clear         vacía la lista\n"
         "  list           muestra la lista        status        estado y búfer\n"
         "  play | pause | stop | next | prev      help | quit");
}

static int handle_line(Player *p, char *line) {
    while (isspace((unsigned char)*line)) line++;
    size_t l = strlen(line);
    while (l && isspace((unsigned char)line[l - 1])) line[--l] = 0;
    if (!*line) return 0;

    char cmd[16]; int a, b, off = 0;
    if (sscanf(line, "%15s%n", cmd, &off) < 1) return 0;
    const char *rest = line + off; while (isspace((unsigned char)*rest)) rest++;

    if      (!strcmp(cmd, "add"))   { if (*rest) player_add(p, rest); else puts("uso: add <ruta>"); }
    else if (!strcmp(cmd, "rm"))    { if (sscanf(rest, "%d", &a) == 1) player_remove(p, a); else puts("uso: rm <n>"); }
    else if (!strcmp(cmd, "mv"))    { if (sscanf(rest, "%d %d", &a, &b) == 2) player_move(p, a, b); else puts("uso: mv <a> <b>"); }
    else if (!strcmp(cmd, "clear")) player_clear(p);
    else if (!strcmp(cmd, "list") || !strcmp(cmd, "ls")) player_list(p);
    else if (!strcmp(cmd, "play"))  player_play(p);
    else if (!strcmp(cmd, "pause")) player_pause(p);
    else if (!strcmp(cmd, "stop"))  player_stop(p);
    else if (!strcmp(cmd, "next"))  player_next(p);
    else if (!strcmp(cmd, "prev"))  player_prev(p);
    else if (!strcmp(cmd, "status"))player_status(p);
    else if (!strcmp(cmd, "help"))  ayuda();
    else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) return 1;
    else printf("comando desconocido: %s (help)\n", cmd);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    static Player player;
    if (player_init(&player) < 0) { fprintf(stderr, "No se pudo iniciar el reproductor (¿ffplay instalado?)\n"); return 1; }

    g_evfd = player_event_fd(&player);
    struct sigaction sa = {0}; sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    int usar_monitor = 1;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-monitor")) usar_monitor = 0;
    int mon = usar_monitor ? monitor_start(&player) : -1;   /* -1: sin TTY o desactivado */

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--no-monitor")) player_add(&player, argv[i]);
    ayuda();

    struct pollfd fds[2] = {{0, POLLIN, 0}, {g_evfd, POLLIN, 0}};
    char acc[1024]; size_t len = 0; int done = 0;
    while (!done) {
        printf("> "); fflush(stdout);
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & POLLIN) break;                       /* señal o fallo del audio */
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(0, acc + len, sizeof acc - len - 1);
            if (n <= 0) break;                                    /* EOF (Ctrl-D) */
            len += (size_t)n;
            char *nl;
            while (!done && (nl = memchr(acc, '\n', len))) {
                *nl = 0;
                done = handle_line(&player, acc);
                size_t rest = len - (size_t)(nl - acc) - 1;
                memmove(acc, nl + 1, rest); len = rest;
            }
            if (len >= sizeof acc - 1) len = 0;                   /* línea demasiado larga: descartar */
        }
    }
    puts("\nCerrando...");
    player_quit(&player);                 /* despierta a todos, incluido el monitor */
    if (mon == 0) monitor_stop();
    player_shutdown(&player);
    return 0;
}
