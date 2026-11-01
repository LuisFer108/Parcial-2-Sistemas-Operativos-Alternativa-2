#ifndef MONITOR_H
#define MONITOR_H
#include "player.h"

/* Panel de monitoreo en las 3 primeras líneas de la terminal; los comandos y
 * mensajes siguen apareciendo debajo (región de scroll ANSI).
 * Devuelve 0 si se activó, -1 si no (stdout no es TTY o terminal muy pequeña). */
int  monitor_start(Player *p);
void monitor_stop(void);     /* llamar DESPUÉS de player_quit() y ANTES de player_shutdown() */
#endif
