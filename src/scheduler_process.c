/*
 * scheduler_process.c  (P0)
 * ─────────────────────────
 * Responsabilidades:
 *   - Inicializar memoria compartida, semáforos y mutex POSIX.
 *   - Leer map.txt y cargarlo en shared state.
 *   - Crear P1, P2 y P3 (fork).
 *   - Ciclo principal: tick_thread + scheduler_thread + signal_thread.
 *   - Procesar solicitudes de cambio de prioridad.
 *   - Detectar colisiones publicadas por P2 y gestionar vidas.
 *   - Señalizar fin de juego y esperar hijos.
 */

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

/* ── semáforos nombrados (externos, entre procesos) ── */
static sem_t *sem_pacman = NULL;
static sem_t *sem_enemy  = NULL;

/* ── memoria compartida ── */
static SharedState *shm = NULL;
static int          shm_fd = -1;

/* ── estado del scheduler ── */
static int round_robin_turn = 0;  /* 0=pacman, 1=enemy (para desempate) */

// aging
// Cada tick que un proceso NO gana, acumula "espera". Esa espera se
//  convierte en prioridad efectiva extra, así nunca puede quedar
//  bloqueado indefinidamente sin importar las prioridades base.

#define AGING_FACTOR 2   // puntos de prioridad efectiva por tick de espera
static int wait_pacman = 0;
static int wait_enemy  = 0;

/* ── prototipos de hilos ── */
static void *tick_thread_fn(void *arg);
static void *scheduler_thread_fn(void *arg);
static void *signal_thread_fn(void *arg);

/* ─────────────────────────────────────────────────────────
   Lectura del mapa
   ───────────────────────────────────────────────────────── */
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

    /* Identificar posiciones iniciales */
    for (int r = 0; r < shm->map_rows; r++) {
        for (int c = 0; c < shm->map_cols; c++) {
            char ch = shm->map_grid[r][c];
            if (ch == CELL_PACMAN) {
                shm->pacman_x = c;
                shm->pacman_y = r;
                shm->map_grid[r][c] = CELL_PATH; /* celda libre tras posicionar */
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

/* ─────────────────────────────────────────────────────────
   Inicialización de memoria compartida y sincronización
   ───────────────────────────────────────────────────────── */
static int init_shared(void) {
    /* Limpiar objetos previos */
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

    /* Estado inicial */
    shm->global_tick   = 0;
    shm->max_ticks     = DEFAULT_MAX_TICKS;
    shm->game_over     = 0;
    shm->pacman_lives  = 3;
    shm->pacman_score  = 0;
    shm->pacman_power  = 0;
    shm->prioridad_pacman = 20;
    shm->prioridad_enemy  = 25;

    /* Mutex entre procesos (PTHREAD_PROCESS_SHARED) */
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

    /* Semáforo embebido para renderer (PTHREAD_PROCESS_SHARED) */
    pthread_mutexattr_t dummy; (void)dummy;
    sem_init(&shm->sem_render_ready, 1 /*pshared*/, 0);

    /* Semáforos nombrados para señalizar P1 y P2 */
    sem_pacman = sem_open(SEM_PACMAN, O_CREAT | O_EXCL, 0666, 0);
    if (sem_pacman == SEM_FAILED) { perror("sem_open pacman"); return -1; }
    sem_enemy  = sem_open(SEM_ENEMY,  O_CREAT | O_EXCL, 0666, 0);
    if (sem_enemy == SEM_FAILED) { perror("sem_open enemy"); return -1; }

    LOG("[P0] Memoria compartida y semáforos inicializados");
    return 0;
}

/* ─────────────────────────────────────────────────────────
   Procesamiento de solicitudes de prioridad
   ───────────────────────────────────────────────────────── */
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

/* ─────────────────────────────────────────────────────────
   Procesamiento de colisiones publicadas por P2
   ───────────────────────────────────────────────────────── */
static void process_collision(void) {
    pthread_mutex_lock(&shm->mutex_collision);
    if (!shm->collision_detected) {
        pthread_mutex_unlock(&shm->mutex_collision);
        return;
    }

    /* ¿Pac-Man tiene power-pellet activo? */
    pthread_mutex_lock(&shm->mutex_power);
    int powered = (shm->pacman_power > 0);
    pthread_mutex_unlock(&shm->mutex_power);

    if (!powered) {
        shm->pacman_lives--;
        LOG("[P0] ¡COLISIÓN con fantasma %d! Vidas restantes: %d",
            shm->collision_ghost_id, shm->pacman_lives);
        if (shm->pacman_lives <= 0) {
            shm->game_over = 1;
            snprintf(shm->win_reason, sizeof(shm->win_reason),
                     "Pac-Man sin vidas (tick %d)", shm->global_tick);
        }
    } else {
        LOG("[P0] Colisión detectada pero Pac-Man tiene power-pellet activo — ignorada");
    }
    shm->collision_detected = 0;
    pthread_mutex_unlock(&shm->mutex_collision);
}

/* ─────────────────────────────────────────────────────────
   Tick thread: incrementa global_tick
   ───────────────────────────────────────────────────────── */
static void *tick_thread_fn(void *arg) {
    (void)arg;
    while (!shm->game_over) {
        ms_sleep(TICK_DELAY_MS);
        /* El scheduler_thread usa global_tick; lo incrementamos bajo mutex ligero.
           No necesita mutex pesado porque scheduler_thread solo lo lee, no lo modifica. */
        __atomic_fetch_add(&shm->global_tick, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   Scheduler thread: decide quién ejecuta este tick
   (actualiza round_robin_turn y decide el ganador)
   ───────────────────────────────────────────────────────── */
typedef struct {
    int *winner;     /* 0=pacman, 1=enemy */
    int *ready;      /* flag para signal_thread */
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

    while (!shm->game_over) {
        int cur = __atomic_load_n(&shm->global_tick, __ATOMIC_SEQ_CST);
        if (cur == last_tick) { usleep(1000); continue; }
        last_tick = cur;

        /* procesar solicitudes de prioridad al inicio del tick */
        process_priority_requests();
        process_collision();

        if (shm->game_over) break;

        /* check tick máximo */
        if (cur >= shm->max_ticks) {
            shm->game_over = 1;
            snprintf(shm->win_reason, sizeof(shm->win_reason),
                     "Ticks máximos alcanzados (%d)", shm->max_ticks);
            break;
        }

        /* decrementar power-pellet */
        pthread_mutex_lock(&shm->mutex_power);
        if (shm->pacman_power > 0) shm->pacman_power--;
        pthread_mutex_unlock(&shm->mutex_power);

        /* decidir ganador (prioridad base + aging por espera) */
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
            /* round-robin */
            winner = round_robin_turn;
            round_robin_turn ^= 1;
        }

        /* el ganador resetea su espera; el perdedor acumula otro tick */
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

    /* despertar signal_thread para que pueda salir */
    pthread_mutex_lock(&sched_mtx);
    sched_ready = 1;
    pthread_cond_signal(&sched_cond);
    pthread_mutex_unlock(&sched_mtx);
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   Signal thread: envía el semáforo al proceso ganador
   ───────────────────────────────────────────────────────── */
static void *signal_thread_fn(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&sched_mtx);
        while (!sched_ready) pthread_cond_wait(&sched_cond, &sched_mtx);
        int w = sched_winner;
        sched_ready = 0;
        pthread_mutex_unlock(&sched_mtx);

        if (shm->game_over) break;

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

        /* señalizar renderer en cada tick */
        sem_post(&shm->sem_render_ready);
    }

    /* señal final al renderer para que salga */
    sem_post(&shm->sem_render_ready);
    return NULL;
}

/* ─────────────────────────────────────────────────────────
   Liberación de recursos
   ───────────────────────────────────────────────────────── */
static void cleanup(void) {
    if (sem_pacman) { sem_close(sem_pacman); sem_unlink(SEM_PACMAN); }
    if (sem_enemy)  { sem_close(sem_enemy);  sem_unlink(SEM_ENEMY);  }
    /* SEM_RENDER es embebido en SHM (sem_init), no nombrado — no se hace sem_unlink */

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

/* ─────────────────────────────────────────────────────────
   main de P0
   ───────────────────────────────────────────────────────── */
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

    /* P1 y P2 leen esta variable de entorno para saber de qué carpeta
       de caso (Caso1/Caso2/Caso3) deben tomar sus archivos de movimiento.
       setenv() hace que la hereden automáticamente en fork()+execlp(). */
    setenv("MAP_DIR", map_dir, 1);

    LOG("[P0] Iniciando scheduler_process");

    if (init_shared() < 0) { cleanup(); return 1; }
    shm->max_ticks = max_ticks;

    if (load_map(map_path) < 0) { cleanup(); return 1; }

    /* Crear P3 primero (renderer) */
    pid_t pid_p3 = fork();
    if (pid_p3 < 0) { perror("fork p3"); cleanup(); return 1; }
    if (pid_p3 == 0) {
        execlp("./renderer_process", "renderer_process", NULL);
        perror("execlp renderer_process"); exit(1);
    }

    /* Crear P1 */
    pid_t pid_p1 = fork();
    if (pid_p1 < 0) { perror("fork p1"); cleanup(); return 1; }
    if (pid_p1 == 0) {
        execlp("./pacman_process", "pacman_process", NULL);
        perror("execlp pacman_process"); exit(1);
    }

    /* Crear P2 */
    pid_t pid_p2 = fork();
    if (pid_p2 < 0) { perror("fork p2"); cleanup(); return 1; }
    if (pid_p2 == 0) {
        execlp("./enemy_process", "enemy_process", NULL);
        perror("execlp enemy_process"); exit(1);
    }

    LOG("[P0] Procesos creados: P1=%d P2=%d P3=%d", pid_p1, pid_p2, pid_p3);

    /* Pequeña pausa para que los hijos abran la SHM */
    ms_sleep(200);

    /* Lanzar hilos de P0 */
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

    /* Liberar semáforos para que P1/P2 puedan terminar (múltiples posts por seguridad) */
    sem_post(sem_pacman); sem_post(sem_pacman);
    sem_post(sem_enemy);  sem_post(sem_enemy);
    /* señal final al renderer */
    sem_post(&shm->sem_render_ready);

    /* Esperar hijos */
    waitpid(pid_p1, NULL, 0);
    waitpid(pid_p2, NULL, 0);
    waitpid(pid_p3, NULL, 0);

    cleanup();
    LOG("[P0] Todos los procesos terminaron. ¡Fin!");
    return 0;
}