#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "shared.h"
#include "utils.h"

static sem_t *sem_pacman = NULL;
static sem_t *sem_enemy  = NULL;

static SharedState *shm = NULL;
static int          shm_fd = -1;

static int round_robin_turn = 0;
static int collision_cooldown = 0; /* ticks restantes de inmunidad post-colisión */

#define AGING_FACTOR 2  
static int wait_pacman = 0;
static int wait_enemy  = 0;

static void *tick_thread_fn(void *arg);
static void *scheduler_thread_fn(void *arg);
static void *signal_thread_fn(void *arg);

static int load_map(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen map.txt"); return -1; }

    char line[MAX_X + 4];
    int row = 0;
    while (fgets(line, sizeof(line), f) && row < MAX_Y) {
        int len = (int)strlen(line);
        /* quitar \n */
        if (len > 0 && line[len-1] == '\n') { line[--len] = '\0'; }
        if (len > 0 && line[len-1] == '\r') { line[--len] = '\0'; }
        if (len == 0) continue;

        if (shm->map_cols == 0) shm->map_cols = len;
        memcpy(shm->map_grid[row], line, len);
        row++;
    }
    shm->map_rows = row;
    fclose(f);

    for (int r = 0; r < shm->map_rows; r++) {
        for (int c = 0; c < shm->map_cols; c++) {
            char ch = shm->map_grid[r][c];
            if (ch == CELL_PACMAN) {
                shm->pacman_x = c;
                shm->pacman_y = r;
                shm->map_grid[r][c] = CELL_PATH; 
            } else if (ch == CELL_GHOST_A) {
                shm->ghost_x[0] = c; shm->ghost_y[0] = r;
                shm->ghost_active[0] = 1;
                shm->map_grid[r][c] = CELL_PATH;
            } else if (ch == CELL_GHOST_B) {
                shm->ghost_x[1] = c; shm->ghost_y[1] = r;
                shm->ghost_active[1] = 1;
                shm->map_grid[r][c] = CELL_PATH;
            } else if (ch == CELL_GHOST_C) {
                shm->ghost_x[2] = c; shm->ghost_y[2] = r;
                shm->ghost_active[2] = 1;
                shm->map_grid[r][c] = CELL_PATH;
            } else if (ch == CELL_GHOST_D) {
                shm->ghost_x[3] = c; shm->ghost_y[3] = r;
                shm->ghost_active[3] = 1;
                shm->map_grid[r][c] = CELL_PATH;
            }
        }
    }
    LOG("[P0] Mapa cargado: %d filas x %d cols", shm->map_rows, shm->map_cols);
    LOG("[P0] Pac-Man inicial: (%d,%d)", shm->pacman_x, shm->pacman_y);
    return 0;
}

static int init_shared(void) {
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_PACMAN);
    sem_unlink(SEM_ENEMY);
    sem_unlink(SEM_RENDER);

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open"); return -1; }
    if (ftruncate(shm_fd, sizeof(SharedState)) < 0) { perror("ftruncate"); return -1; }

    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return -1; }
    memset(shm, 0, sizeof(SharedState));

    shm->global_tick   = 0;
    shm->max_ticks     = DEFAULT_MAX_TICKS;
    shm->game_over     = 0;
    shm->pacman_lives  = 3;
    shm->pacman_score  = 0;
    shm->pacman_power  = 0;
    shm->prioridad_pacman = 20;
    shm->prioridad_enemy  = 25;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&shm->mutex_pacman_pos,  &ma);
    pthread_mutex_init(&shm->mutex_collision,   &ma);
    pthread_mutex_init(&shm->mutex_priority,    &ma);
    pthread_mutex_init(&shm->mutex_ghost_pos,   &ma);
    pthread_mutex_init(&shm->mutex_map,         &ma);
    pthread_mutex_init(&shm->mutex_power,       &ma);
    pthread_mutexattr_destroy(&ma);

    pthread_mutexattr_t dummy; (void)dummy;
    sem_init(&shm->sem_render_ready, 1, 0);

    sem_pacman = sem_open(SEM_PACMAN, O_CREAT | O_EXCL, 0666, 0);
    if (sem_pacman == SEM_FAILED) { perror("sem_open pacman"); return -1; }
    sem_enemy  = sem_open(SEM_ENEMY,  O_CREAT | O_EXCL, 0666, 0);
    if (sem_enemy == SEM_FAILED) { perror("sem_open enemy"); return -1; }

    LOG("[P0] Memoria compartida y semáforos inicializados");
    return 0;
}

static void process_priority_requests(void) {
    pthread_mutex_lock(&shm->mutex_priority);

    if (shm->priority_request_active) {
        int np = shm->pending_priority_pacman;
        if (np >= 1 && np <= 100) {
            shm->prioridad_pacman = np;
            LOG("[P0] Prioridad Pac-Man actualizada → %d", np);
        }
        shm->priority_request_active = 0;
        shm->pending_priority_pacman = 0;
    }

    if (shm->enemy_priority_request_active) {
        int ne = shm->pending_priority_enemy;
        if (ne >= 1 && ne <= 100) {
            shm->prioridad_enemy = ne;
            LOG("[P0] Prioridad Enemy actualizada → %d", ne);
        }
        shm->enemy_priority_request_active = 0;
        shm->pending_priority_enemy = 0;
    }

    pthread_mutex_unlock(&shm->mutex_priority);
}

static void process_collision(void) {
    /* Cooldown: ignorar colisiones por 3 ticks tras procesar una (ghost sigue en misma celda) */
    if (collision_cooldown > 0) {
        collision_cooldown--;
        pthread_mutex_lock(&shm->mutex_collision);
        shm->collision_detected = 0;
        pthread_mutex_unlock(&shm->mutex_collision);
        return;
    }

    pthread_mutex_lock(&shm->mutex_collision);
    if (!shm->collision_detected) {
        pthread_mutex_unlock(&shm->mutex_collision);
        return;
    }

    pthread_mutex_lock(&shm->mutex_power);
    int powered = (shm->pacman_power > 0);
    pthread_mutex_unlock(&shm->mutex_power);

    if (!powered) {
        shm->pacman_lives--;
        LOG("[P0] ¡COLISIÓN con fantasma %d! Vidas restantes: %d",
            shm->collision_ghost_id, shm->pacman_lives);
        if (shm->pacman_lives <= 0) {
            GAME_OVER_SET(shm);
            snprintf(shm->win_reason, sizeof(shm->win_reason),
                     "Pac-Man sin vidas (tick %d)", shm->global_tick);
        }
    } else {
        LOG("[P0] Colisión detectada pero Pac-Man tiene power-pellet activo — ignorada");
    }
    shm->collision_detected = 0;
    if (!GAME_OVER_GET(shm)) collision_cooldown = 3; /* 3 ticks de gracia */
    pthread_mutex_unlock(&shm->mutex_collision);
}

static void *tick_thread_fn(void *arg) {
    (void)arg;
    while (!GAME_OVER_GET(shm)) {
        ms_sleep(TICK_DELAY_MS);
        __atomic_fetch_add(&shm->global_tick, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

typedef struct {
    int *winner;    
    int *ready;     
    pthread_mutex_t *mtx;
    pthread_cond_t  *cond;
} SchedArgs;

static int  sched_winner = 0;
static int  sched_ready  = 0;
static pthread_mutex_t sched_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sched_cond = PTHREAD_COND_INITIALIZER;

static void *scheduler_thread_fn(void *arg) {
    (void)arg;
    int last_tick = -1;

    while (!GAME_OVER_GET(shm)) {
        int cur = __atomic_load_n(&shm->global_tick, __ATOMIC_SEQ_CST);
        if (cur == last_tick) { usleep(1000); continue; }
        last_tick = cur;

        process_priority_requests();
        process_collision();

        if (GAME_OVER_GET(shm)) break;

        if (cur >= shm->max_ticks) {
            GAME_OVER_SET(shm);
            snprintf(shm->win_reason, sizeof(shm->win_reason),
                     "Ticks máximos alcanzados (%d)", shm->max_ticks);
            break;
        }

        pthread_mutex_lock(&shm->mutex_power);
        if (shm->pacman_power > 0) shm->pacman_power--;
        pthread_mutex_unlock(&shm->mutex_power);

        int pp, pe;
        pthread_mutex_lock(&shm->mutex_priority);
        pp = shm->prioridad_pacman;
        pe = shm->prioridad_enemy;
        pthread_mutex_unlock(&shm->mutex_priority);

        int eff_pp = pp + AGING_FACTOR * wait_pacman;
        int eff_pe = pe + AGING_FACTOR * wait_enemy;

        int winner;
        if (eff_pp > eff_pe)      winner = 0;
        else if (eff_pe > eff_pp) winner = 1;
        else {
            winner = round_robin_turn;
            round_robin_turn ^= 1;
        }

        if (winner == 0) { wait_pacman = 0; wait_enemy++; }
        else              { wait_enemy = 0;  wait_pacman++; }

        LOG("[P0] tick=%d prioridad_efectiva pacman=%d(base %d,wait %d) enemy=%d(base %d,wait %d)",
            cur, eff_pp, pp, wait_pacman, eff_pe, pe, wait_enemy);

        pthread_mutex_lock(&sched_mtx);
        sched_winner = winner;
        sched_ready  = 1;
        pthread_cond_signal(&sched_cond);
        pthread_mutex_unlock(&sched_mtx);
    }

    pthread_mutex_lock(&sched_mtx);
    sched_ready = 1;
    pthread_cond_signal(&sched_cond);
    pthread_mutex_unlock(&sched_mtx);
    return NULL;
}

static void *signal_thread_fn(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&sched_mtx);
        while (!sched_ready) pthread_cond_wait(&sched_cond, &sched_mtx);
        int w = sched_winner;
        sched_ready = 0;
        pthread_mutex_unlock(&sched_mtx);

        if (GAME_OVER_GET(shm)) break;

        if (w == 0) {
            sem_post(sem_pacman);
            LOG("[P0] tick=%d → turno PACMAN (P%d=%d, E%d=%d)",
                shm->global_tick, shm->prioridad_pacman,
                shm->prioridad_pacman, shm->prioridad_enemy,
                shm->prioridad_enemy);
        } else {
            sem_post(sem_enemy);
            LOG("[P0] tick=%d → turno ENEMY (P%d=%d, E%d=%d)",
                shm->global_tick, shm->prioridad_pacman,
                shm->prioridad_pacman, shm->prioridad_enemy,
                shm->prioridad_enemy);
        }

        sem_post(&shm->sem_render_ready);
    }

    sem_post(&shm->sem_render_ready);
    return NULL;
}

static void cleanup(void) {
    if (sem_pacman) { sem_close(sem_pacman); sem_unlink(SEM_PACMAN); }
    if (sem_enemy)  { sem_close(sem_enemy);  sem_unlink(SEM_ENEMY);  }

    if (shm && shm != MAP_FAILED) {
        pthread_mutex_destroy(&shm->mutex_pacman_pos);
        pthread_mutex_destroy(&shm->mutex_collision);
        pthread_mutex_destroy(&shm->mutex_priority);
        pthread_mutex_destroy(&shm->mutex_ghost_pos);
        pthread_mutex_destroy(&shm->mutex_map);
        pthread_mutex_destroy(&shm->mutex_power);
        sem_destroy(&shm->sem_render_ready);
        munmap(shm, sizeof(SharedState));
    }
    if (shm_fd >= 0) close(shm_fd);
    shm_unlink(SHM_NAME);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <carpeta_caso> [max_ticks]\n", argv[0]);
        fprintf(stderr, "Ej:  %s maps/Caso1 300\n", argv[0]);
        return 1;
    }
    const char *map_dir   = argv[1];
    int         max_ticks = (argc > 2) ? atoi(argv[2]) : DEFAULT_MAX_TICKS;

    char map_path[256];
    snprintf(map_path, sizeof(map_path), "%s/map.txt", map_dir);
    setenv("MAP_DIR", map_dir, 1);

    LOG("[P0] Iniciando scheduler_process");

    if (init_shared() < 0) { cleanup(); return 1; }
    shm->max_ticks = max_ticks;

    if (load_map(map_path) < 0) { cleanup(); return 1; }

    pid_t pid_p3 = fork();
    if (pid_p3 < 0) { perror("fork p3"); cleanup(); return 1; }
    if (pid_p3 == 0) {
        execlp("./renderer_process", "renderer_process", NULL);
        perror("execlp renderer_process"); exit(1);
    }

    pid_t pid_p1 = fork();
    if (pid_p1 < 0) { perror("fork p1"); cleanup(); return 1; }
    if (pid_p1 == 0) {
        execlp("./pacman_process", "pacman_process", NULL);
        perror("execlp pacman_process"); exit(1);
    }

    pid_t pid_p2 = fork();
    if (pid_p2 < 0) { perror("fork p2"); cleanup(); return 1; }
    if (pid_p2 == 0) {
        execlp("./enemy_process", "enemy_process", NULL);
        perror("execlp enemy_process"); exit(1);
    }

    LOG("[P0] Procesos creados: P1=%d P2=%d P3=%d", pid_p1, pid_p2, pid_p3);

    ms_sleep(200);

    pthread_t thr_tick, thr_sched, thr_signal;
    pthread_create(&thr_tick,   NULL, tick_thread_fn,      NULL);
    pthread_create(&thr_sched,  NULL, scheduler_thread_fn, NULL);
    pthread_create(&thr_signal, NULL, signal_thread_fn,    NULL);

    pthread_join(thr_tick,   NULL);
    pthread_join(thr_sched,  NULL);
    pthread_join(thr_signal, NULL);

    LOG("[P0] Fin del juego: %s", shm->win_reason);
    LOG("[P0] Score final: %d | Vidas: %d | Ticks: %d",
        shm->pacman_score, shm->pacman_lives, shm->global_tick);

    sem_post(sem_pacman); sem_post(sem_pacman);
    sem_post(sem_enemy);  sem_post(sem_enemy);
    sem_post(&shm->sem_render_ready);

    waitpid(pid_p1, NULL, 0);
    waitpid(pid_p2, NULL, 0);
    waitpid(pid_p3, NULL, 0);

    cleanup();
    LOG("[P0] Todos los procesos terminaron. ¡Fin!");
    return 0;
}