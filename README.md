# Reproductor de audio concurrente

Reproductor de audio de línea de comandos en C con hilos POSIX, con gestión concurrente de la lista de reproducción. Proyecto del Parcial 2 de Sistemas Operativos (Alternativa 2: concurrencia y sincronización).
Luis Fernando Bernal Ramirez

**Video Explicativo:** https://drive.google.com/file/d/1t_h6fZQoqFwq74evQYUctDHJlfemfmmU/view?usp=sharing

Puedes agregar, eliminar y reordenar canciones, pausar o saltar de pista **mientras suena**, sin condiciones de carrera ni interbloqueos.

## Características

- Esquema **productor-consumidor** con búfer circular (mutex + variables de condición, sin espera activa)
- **Playlist compartida** protegida con `pthread_rwlock` (lectores-escritores)
- Control inmediato: `play`, `pause`, `stop`, `next`, `prev`
- Transiciones entre pistas sin cortes (gapless)
- **Monitor en tiempo real** del estado, el búfer y la actividad de cada hilo
- Cierre ordenado ante `Ctrl-C` (hilos unidos, procesos hijos recolectados)

## Compilar y ejecutar

```bash
make                                   # compilar
./reproductor cancion1.mp3 cancion2.mp3
make tsan                              # versión con ThreadSanitizer
make clean
```

Opción `--no-monitor` para desactivar el panel (se desactiva solo si la salida no es una terminal; el panel necesita al menos 10 filas).

## Comandos

| Comando | Acción |
| --- | --- |
| `add <ruta>` | Agrega una pista al final |
| `rm <n>` | Elimina la pista n |
| `mv <a> <b>` | Mueve la pista a a la posición b |
| `clear` | Vacía la lista y detiene |
| `list` | Muestra la lista |
| `play` / `pause` / `stop` | Control de reproducción |
| `next` / `prev` | Pista siguiente / anterior |
| `status` | Estado y ocupación del búfer |
| `help` / `quit` | Ayuda / salir |

## Arquitectura

```
ffmpeg --> [Productor] --> [Búfer circular] --> [Consumidor] --> ffplay
                ^                 ^
                | lee             | lee
           [Playlist] <-- escribe -- [CLI]      [Monitor]
```

Cuatro hilos: **CLI** (comandos), **productor** (decodifica con `ffmpeg`), **consumidor** (envía el PCM a `ffplay`) y **monitor** (solo lectura).

| Archivo | Responsabilidad |
| --- | --- |
| `reproductor.c` | CLI: lectura de comandos con `poll` |
| `player.c/.h` | Motor: hilos, procesos auxiliares, comandos, protocolo de locks |
| `ringbuf.c/.h` | Búfer circular con epoch, pausa y cierre |
| `playlist.c/.h` | Lista doblemente enlazada |
| `monitor.c/.h` | Panel de estado en tiempo real |

