/*
 * enemy_process.c  (P2)
 * ─────────────────────
 * Hilos:
 *   enemy_controller_thread  → espera semáforo de P0, despierta ghost threads
 *   ghost_thread_[0..3]      → lee su archivo, mueve su fantasma
 *   pacman_tracker_thread    → copia posición de Pac-Man desde SHM
 *   collision_thread         → detecta colisiones y publica evento
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <pthread.h>
#include <errno.h>

#include "shared.h"
#include "utils.h"

static const char *GHOST_FILES[NUM_GHOSTS] = {
    "maps/ghost_1_moves.txt",
    "maps/ghost_2_moves.txt",
    "maps/ghost_3_moves.txt",
    "maps/ghost_4_moves.txt",
};

/* ── Memoria compartida ── */
static SharedState *shm    = NULL;
static int          shm_fd = -1;
static sem_t       *sem_p2 = NULL;

/* ── Posiciones locales de fantasmas (protegidas por mutex interno) ── */
static int ghost_local_x[NUM_GHOSTS];
static int ghost_local_y[NUM_GHOSTS];
static pthread_mutex_t mtx_ghost_local = PTHREAD_MUTEX_INITIALIZER;

/* ── Copia local de posición de Pac-Man ── */
static int pac_last_x = 0, pac_last_y = 0;
static pthread_mutex_t mtx_pac_local = PTHREAD_MUTEX_INITIALIZER;

/* ── Coordinación controller → ghost threads ── */
static sem_t       sem_ghost_go[NUM_GHOSTS];   /* controller → ghost_i */
static sem_t       sem_ghost_done[NUM_GHOSTS]; /* ghost_i → controller */

/* ── Archivos de movimientos ── */
static FILE *ghost_files[NUM_GHOSTS];

/* ─────────────────────────────────────────────────────────
   Mover fantasma (validar contra mapa)
   ───────────────────────────────────────────────────────── */
static void ghost_apply_move(int id, const char *dir) {
    int dx, dy;
    direction_delta(dir, &dx, &dy);

    pthread_mutex_lock(&mtx_ghost_local);
    int nx = ghost_local_x[id] + dx;
    int ny = ghost_local_y[id] + dy;
    pthread_mutex_unlock(&mtx_ghost_local);

    pthread_mutex_lock(&shm->mutex_map);
    if (nx < 0 || nx >= shm->map_cols || ny < 0 || ny >= shm->map_rows ||
        !is_walkable(shm->map_grid[ny][nx])) {
        pthread_mutex_unlock(&shm->mutex_map);
        LOG("[P2-ghost%d] Movimiento %s bloqueado", id, dir);
        return;
    }
    pthread_mutex_unlock(&shm->mutex_map);

    pthread_mutex_lock(&mtx_ghost_local);
    ghost_local_x[id] = nx;
    ghost_local_y[id] = ny;
    pthread_mutex_unlock(&mtx_ghost_local);

    /* Publicar en SHM para renderer */
    pthread_mutex_lock(&shm->mutex_ghost_pos);
    shm->ghost_x[id] = nx;
    shm->ghost_y[id] = ny;
    pthread_mutex_unlock(&shm->mutex_ghost_pos);

    LOG("[P2-ghost%d] → (%d,%d)", id, nx, ny);
}

/* ─────────────────────────────────────────────────────────
   ghost_thread_i
   ───────────────────────────────────────────────────────── */
typedef struct { int id; } GhostArg;

static void *ghost_thread_fn(void *arg) {
    int id = ((GhostArg*)arg)->id;
    char line[64];

    while (!shm->game_over) {
        /* esperar señal del controller */
        sem_wait(&sem_ghost_go[id]);
        if (shm->game_over) { sem_post(&sem_ghost_done[id]); break; }

        if (!ghost_files[id]) {
            sem_post(&sem_ghost_done[id]);
            continue;
        }

        /* leer siguiente instrucción */
        if (!fgets(line, sizeof(line), ghost_files[id])) {
            LOG("[P2-ghost%d] Sin más instrucciones", id);
            fclose(ghost_files[id]);
            ghost_files[id] = NULL;
            sem_post(&sem_ghost_done[id]);
            continue;
        }

        /* trim */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r'||line[len-1]==' '))
            line[--len] = '\0';

        if (strncmp(line, "SET_PRIORITY", 12) == 0) {
            int prio = 0;
            sscanf(line + 12, " %d", &prio);
            pthread_mutex_lock(&shm->mutex_priority);
            shm->pending_priority_enemy        = prio;
            shm->enemy_priority_request_active = 1;
            pthread_mutex_unlock(&shm->mutex_priority);
            LOG("[P2-ghost%d] SET_PRIORITY %d → enviado a P0", id, prio);
        } else {
            ghost_apply_move(id, line);
        }

        sem_post(&sem_ghost_done[id]);
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   enemy_controller_thread
   ───────────────────────────────────────────────────────── */
static void *enemy_controller_thread(void *arg) {
    (void)arg;
    while (!shm->game_over) {
        /* esperar turno de P0 */
        sem_wait(sem_p2);
        if (shm->game_over) break;

        LOG("[P2-ctrl] Turno recibido — despertando ghost threads");

        /* despertar todos los ghost threads */
        for (int i = 0; i < NUM_GHOSTS; i++) {
            if (shm->ghost_active[i]) sem_post(&sem_ghost_go[i]);
        }

        /* esperar que todos terminen */
        for (int i = 0; i < NUM_GHOSTS; i++) {
            if (shm->ghost_active[i]) sem_wait(&sem_ghost_done[i]);
        }

        LOG("[P2-ctrl] Todos los fantasmas completaron su movimiento");
    }

    /* liberar ghost threads bloqueados */
    for (int i = 0; i < NUM_GHOSTS; i++) sem_post(&sem_ghost_go[i]);
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   pacman_tracker_thread
   ───────────────────────────────────────────────────────── */
static void *pacman_tracker_thread(void *arg) {
    (void)arg;
    while (!shm->game_over) {
        int px, py;
        pthread_mutex_lock(&shm->mutex_pacman_pos);
        px = shm->pacman_x;
        py = shm->pacman_y;
        pthread_mutex_unlock(&shm->mutex_pacman_pos);

        pthread_mutex_lock(&mtx_pac_local);
        pac_last_x = px;
        pac_last_y = py;
        pthread_mutex_unlock(&mtx_pac_local);

        usleep(20000); /* 20 ms */
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   collision_thread
   ───────────────────────────────────────────────────────── */
static void *collision_thread_fn(void *arg) {
    (void)arg;
    while (!shm->game_over) {
        usleep(15000); /* revisar cada 15 ms */

        pthread_mutex_lock(&mtx_pac_local);
        int px = pac_last_x;
        int py = pac_last_y;
        pthread_mutex_unlock(&mtx_pac_local);

        pthread_mutex_lock(&mtx_ghost_local);
        for (int i = 0; i < NUM_GHOSTS; i++) {
            if (!shm->ghost_active[i]) continue;
            if (ghost_local_x[i] == px && ghost_local_y[i] == py) {
                pthread_mutex_unlock(&mtx_ghost_local);
                /* publicar colisión */
                pthread_mutex_lock(&shm->mutex_collision);
                if (!shm->collision_detected) {
                    shm->collision_detected  = 1;
                    shm->collision_tick      = shm->global_tick;
                    shm->collision_ghost_id  = i;
                    LOG("[P2-collision] ¡Colisión Pac-Man con fantasma %d en (%d,%d)!",
                        i, px, py);
                }
                pthread_mutex_unlock(&shm->mutex_collision);
                pthread_mutex_lock(&mtx_ghost_local);
                break;
            }
        }
        pthread_mutex_unlock(&mtx_ghost_local);
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   main de P2
   ───────────────────────────────────────────────────────── */
int main(void) {
    LOG("[P2] Iniciando enemy_process");

    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) { perror("[P2] shm_open"); return 1; }
    shm = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("[P2] mmap"); return 1; }

    sem_p2 = sem_open(SEM_ENEMY, 0);
    if (sem_p2 == SEM_FAILED) { perror("[P2] sem_open"); return 1; }

    /* Inicializar posiciones locales desde SHM */
    for (int i = 0; i < NUM_GHOSTS; i++) {
        ghost_local_x[i] = shm->ghost_x[i];
        ghost_local_y[i] = shm->ghost_y[i];
        sem_init(&sem_ghost_go[i],   0, 0);
        sem_init(&sem_ghost_done[i], 0, 0);
        ghost_files[i] = fopen(GHOST_FILES[i], "r");
        if (!ghost_files[i])
            LOG("[P2] Advertencia: no se pudo abrir %s", GHOST_FILES[i]);
    }

    pac_last_x = shm->pacman_x;
    pac_last_y = shm->pacman_y;

    /* Lanzar hilos */
    pthread_t thr_ctrl, thr_tracker, thr_collision;
    pthread_t thr_ghost[NUM_GHOSTS];
    static GhostArg ghost_args[NUM_GHOSTS];

    pthread_create(&thr_ctrl,      NULL, enemy_controller_thread, NULL);
    pthread_create(&thr_tracker,   NULL, pacman_tracker_thread,   NULL);
    pthread_create(&thr_collision, NULL, collision_thread_fn,     NULL);

    for (int i = 0; i < NUM_GHOSTS; i++) {
        ghost_args[i].id = i;
        pthread_create(&thr_ghost[i], NULL, ghost_thread_fn, &ghost_args[i]);
    }

    pthread_join(thr_ctrl,      NULL);
    pthread_join(thr_tracker,   NULL);
    pthread_join(thr_collision, NULL);
    for (int i = 0; i < NUM_GHOSTS; i++) pthread_join(thr_ghost[i], NULL);

    /* Limpiar */
    for (int i = 0; i < NUM_GHOSTS; i++) {
        sem_destroy(&sem_ghost_go[i]);
        sem_destroy(&sem_ghost_done[i]);
        if (ghost_files[i]) fclose(ghost_files[i]);
    }
    sem_close(sem_p2);
    munmap(shm, sizeof(SharedState));
    close(shm_fd);
    LOG("[P2] enemy_process terminado");
    return 0;
}
