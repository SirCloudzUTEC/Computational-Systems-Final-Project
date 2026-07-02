#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "shared.h"

/* ── logging a archivo (evita mezclar con ncurses en terminal) ──
 *
 * Todos los procesos (P0/P1/P2/P3) escriben en pacman.log en lugar
 * de stderr, así ncurses puede dibujar sin interferencias.
 *
 * Para ver los logs en tiempo real: tail -f pacman.log
 */
static FILE *_log_file = NULL;

static inline FILE *log_get_file(void) {
    if (!_log_file) {
        _log_file = fopen("pacman.log", "a");
        if (!_log_file) _log_file = stderr; /* fallback */
    }
    return _log_file;
}

#define LOG(fmt, ...) \
    do { \
        struct timespec _ts; \
        clock_gettime(CLOCK_MONOTONIC, &_ts); \
        FILE *_lf = log_get_file(); \
        fprintf(_lf, "[%ld.%03ld] " fmt "\n", \
                (long)_ts.tv_sec, (long)(_ts.tv_nsec / 1000000), ##__VA_ARGS__); \
        fflush(_lf); \
    } while(0)

/* ── delay configurable entre ticks (ms) ── */
#define TICK_DELAY_MS 0

static inline void ms_sleep(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── validar celda transitable ── */
static inline int is_walkable(char c) {
    return (c == CELL_PATH || c == CELL_PELLET ||
            c == CELL_PACMAN || c == CELL_GHOST_A ||
            c == CELL_GHOST_B || c == CELL_GHOST_C ||
            c == CELL_GHOST_D);
}

/* ── aplicar delta de dirección ── */
static inline void direction_delta(const char *dir, int *dx, int *dy) {
    *dx = 0; *dy = 0;
    if      (strcmp(dir, "UP")    == 0) *dy = -1;
    else if (strcmp(dir, "DOWN")  == 0) *dy =  1;
    else if (strcmp(dir, "LEFT")  == 0) *dx = -1;
    else if (strcmp(dir, "RIGHT") == 0) *dx =  1;
}

#endif /* UTILS_H */
