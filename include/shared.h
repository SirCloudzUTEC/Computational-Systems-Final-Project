#ifndef SHARED_H
#define SHARED_H

#include <semaphore.h>
#include <pthread.h>
#include <stdint.h>

/* ── dimensiones del mapa ── */
#define MAX_X        30
#define MAX_Y        20
#define NUM_GHOSTS    4

/* ── celdas de mapa ── */
#define CELL_WALL       'X'
#define CELL_PATH       'O'
#define CELL_PELLET     '*'
#define CELL_PACMAN     'P'
#define CELL_GHOST_A    'A'
#define CELL_GHOST_B    'B'
#define CELL_GHOST_C    'C'
#define CELL_GHOST_D    'D'

/* ── ticks máximos por defecto ── */
#define DEFAULT_MAX_TICKS 300

/* ── nombre del segmento de memoria compartida ── */
#define SHM_NAME   "/pacman_shm"
#define SEM_PACMAN "/pacman_sem_p1"
#define SEM_ENEMY  "/pacman_sem_p2"
#define SEM_RENDER "/pacman_sem_p3"

/* ── estado de power-pellet ── */
#define POWER_DURATION 10   /* ticks que dura el power-pellet */

/* ────────────────────────────────────────────────
   Estructura principal en memoria compartida
   ──────────────────────────────────────────────── */
typedef struct {
    /* ── tick y control global ── */
    int  global_tick;
    int  max_ticks;
    int  game_over;          /* 0=corriendo, 1=game_over */
    char win_reason[64];     /* mensaje final */

    /* ── Pac-Man ── */
    int  pacman_x;
    int  pacman_y;
    int  pacman_score;
    int  pacman_lives;
    int  pacman_power;       /* ticks restantes de power-pellet (0 = normal) */

    /* ── colisiones (escrito por P2, leído por P0) ── */
    int  collision_detected;
    int  collision_tick;
    int  collision_ghost_id;

    /* ── prioridades ── */
    int  prioridad_pacman;
    int  prioridad_enemy;

    /* ── solicitudes de cambio de prioridad ── */
    int  pending_priority_pacman;
    int  priority_request_active;
    int  pending_priority_enemy;
    int  enemy_priority_request_active;

    /* ── mapa (incluye pellets, comida, paredes) ── */
    char map_grid[MAX_Y][MAX_X];
    int  map_rows;
    int  map_cols;

    /* ── fantasmas (para P3) ── */
    int  ghost_x[NUM_GHOSTS];
    int  ghost_y[NUM_GHOSTS];
    int  ghost_active[NUM_GHOSTS];  /* 1=vivo, 0=inactivo */

    /* ── mutex y semáforos embebidos ── */
    pthread_mutex_t mutex_pacman_pos;   /* protege pacman_x/y/score */
    pthread_mutex_t mutex_collision;    /* protege collision_* */
    pthread_mutex_t mutex_priority;     /* protege pending_priority_* */
    pthread_mutex_t mutex_ghost_pos;    /* protege ghost_x[]/ghost_y[] */
    pthread_mutex_t mutex_map;          /* protege map_grid */
    pthread_mutex_t mutex_power;        /* protege pacman_power */

    /* ── semáforo de render (señal de P0 a P3) ── */
    sem_t sem_render_ready;

} SharedState;

#endif /* SHARED_H */
