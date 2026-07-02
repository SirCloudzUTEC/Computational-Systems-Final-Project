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

static char move_file[256];
#define QUEUE_SIZE  256

typedef struct {
    char moves[QUEUE_SIZE][32];
    int  head, tail, count;
    pthread_mutex_t mtx;
    sem_t           avail;   
} MoveQueue;

static MoveQueue mq;

static int local_x, local_y, local_score;

static SharedState *shm = NULL;
static int          shm_fd = -1;
static sem_t       *sem_p1 = NULL;

static int pub_x, pub_y, pub_score;
static int pub_pending = 0;
static pthread_mutex_t pub_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t           pub_sem;

static void queue_init(void) {
    memset(&mq, 0, sizeof(mq));
    pthread_mutex_init(&mq.mtx, NULL);
    sem_init(&mq.avail, 0, 0);
}

static int queue_push(const char *move) {
    pthread_mutex_lock(&mq.mtx);
    if (mq.count >= QUEUE_SIZE) {
        pthread_mutex_unlock(&mq.mtx);
        return -1; /* cola llena */
    }
    strncpy(mq.moves[mq.tail], move, 31);
    mq.tail = (mq.tail + 1) % QUEUE_SIZE;
    mq.count++;
    pthread_mutex_unlock(&mq.mtx);
    sem_post(&mq.avail);
    return 0;
}

static int queue_pop(char *out, size_t outsz) {
    sem_wait(&mq.avail);
    pthread_mutex_lock(&mq.mtx);
    if (mq.count == 0) {
        pthread_mutex_unlock(&mq.mtx);
        return -1;
    }
    strncpy(out, mq.moves[mq.head], outsz - 1);
    out[outsz-1] = '\0';
    mq.head = (mq.head + 1) % QUEUE_SIZE;
    mq.count--;
    pthread_mutex_unlock(&mq.mtx);
    return 0;
}

static void *movement_reader_thread(void *arg) {
    (void)arg;
    FILE *f = fopen(move_file, "r");
    if (!f) {
        LOG("[P1-reader] No se pudo abrir %s", move_file);
        queue_push("EOF");
        return NULL;
    }

    char line[64];
    while (fgets(line, sizeof(line), f)) {
        if (GAME_OVER_GET(shm)) break;
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';
        if (len == 0) continue;
        while (1) {
            pthread_mutex_lock(&mq.mtx);
            int full = (mq.count >= QUEUE_SIZE);
            pthread_mutex_unlock(&mq.mtx);
            if (!full) break;
            usleep(5000);
        }
        queue_push(line);
        LOG("[P1-reader] Encolado: '%s'", line);
    }
    fclose(f);
    queue_push("EOF");
    LOG("[P1-reader] Fin de archivo de movimientos");
    return NULL;
}

static void apply_move(const char *dir) {
    int dx, dy;
    direction_delta(dir, &dx, &dy);
    int nx = local_x + dx;
    int ny = local_y + dy;

    pthread_mutex_lock(&shm->mutex_map);
    if (nx < 0 || nx >= shm->map_cols || ny < 0 || ny >= shm->map_rows) {
        pthread_mutex_unlock(&shm->mutex_map);
        LOG("[P1-exec] Movimiento %s fuera de mapa — ignorado", dir);
        return;
    }
    char cell = shm->map_grid[ny][nx];
    if (!is_walkable(cell)) {
        pthread_mutex_unlock(&shm->mutex_map);
        LOG("[P1-exec] Movimiento %s bloqueado por pared", dir);
        return;
    }

    if (cell == CELL_PELLET) {
        shm->map_grid[ny][nx] = CELL_PATH;
        local_score += 50;

        pthread_mutex_lock(&shm->mutex_power);
        shm->pacman_power = POWER_DURATION;
        pthread_mutex_unlock(&shm->mutex_power);

        LOG("[P1-exec] *** POWER PELLET! Power activo por %d ticks ***", POWER_DURATION);
    } else {
        local_score += 10;
    }
    pthread_mutex_unlock(&shm->mutex_map);

    local_x = nx;
    local_y = ny;
    LOG("[P1-exec] Pac-Man → (%d,%d) score=%d", local_x, local_y, local_score);
}

static void *movement_executor_thread(void *arg) {
    (void)arg;
    char move[32];

    while (!GAME_OVER_GET(shm)) {
        sem_wait(sem_p1);
        if (GAME_OVER_GET(shm)) break;

        if (queue_pop(move, sizeof(move)) < 0) {
            LOG("[P1-exec] Cola vacía inesperada");
            continue;
        }

        if (strcmp(move, "EOF") == 0) {
            LOG("[P1-exec] Sin más movimientos");
            pthread_mutex_lock(&shm->mutex_collision);
            if (!GAME_OVER_GET(shm)) {
                GAME_OVER_SET(shm);
                snprintf(shm->win_reason, sizeof(shm->win_reason),
                        "Pac-Man sin instrucciones (tick %d)", shm->global_tick);
            }
            pthread_mutex_unlock(&shm->mutex_collision);
            break;
        }
        if (strncmp(move, "SET_PRIORITY", 12) == 0) {
            int prio = 0;
            sscanf(move + 12, " %d", &prio);
            pthread_mutex_lock(&shm->mutex_priority);
            shm->pending_priority_pacman = prio;
            shm->priority_request_active  = 1;
            pthread_mutex_unlock(&shm->mutex_priority);
            LOG("[P1-exec] Solicitud SET_PRIORITY %d enviada a P0", prio);
            goto publish;
        }

        apply_move(move);

    publish:
        pthread_mutex_lock(&pub_mtx);
        pub_x = local_x;
        pub_y = local_y;
        pub_score = local_score;
        pub_pending = 1;
        pthread_mutex_unlock(&pub_mtx);
        sem_post(&pub_sem);
    }
    sem_post(&pub_sem); 
    return NULL;
}

static void *pacman_publisher_thread(void *arg) {
    (void)arg;
    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        int r = sem_timedwait(&pub_sem, &ts);
        if (r < 0) {
            if (GAME_OVER_GET(shm)) break;
            continue;
        }
        pthread_mutex_lock(&pub_mtx);
        if (!pub_pending) {
            pthread_mutex_unlock(&pub_mtx);
            if (GAME_OVER_GET(shm)) break;
            continue;
        }
        int px = pub_x, py = pub_y, ps = pub_score;
        pub_pending = 0;
        pthread_mutex_unlock(&pub_mtx);

        pthread_mutex_lock(&shm->mutex_pacman_pos);
        shm->pacman_x     = px;
        shm->pacman_y     = py;
        shm->pacman_score = ps;
        pthread_mutex_unlock(&shm->mutex_pacman_pos);

        LOG("[P1-pub] Publicado: (%d,%d) score=%d", px, py, ps);

        if (GAME_OVER_GET(shm)) break;
    }
    return NULL;
}


int main(void) {
    LOG("[P1] Iniciando pacman_process");

    const char *dir = getenv("MAP_DIR");
    if (!dir) {
        fprintf(stderr, "[P1] Falta variable de entorno MAP_DIR\n");
        return 1;
    }
    snprintf(move_file, sizeof(move_file), "%s/pacman_moves.txt", dir);

    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) { perror("[P1] shm_open"); return 1; }
    shm = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("[P1] mmap"); return 1; }

    sem_p1 = sem_open(SEM_PACMAN, 0);
    if (sem_p1 == SEM_FAILED) { perror("[P1] sem_open"); return 1; }

    /* Estado local inicial desde SHM */
    local_x = shm->pacman_x;
    local_y = shm->pacman_y;
    local_score = 0;

    queue_init();
    sem_init(&pub_sem, 0, 0);

    pthread_t thr_reader, thr_exec, thr_pub;
    pthread_create(&thr_reader, NULL, movement_reader_thread,  NULL);
    pthread_create(&thr_exec,   NULL, movement_executor_thread, NULL);
    pthread_create(&thr_pub,    NULL, pacman_publisher_thread,  NULL);

    pthread_join(thr_reader, NULL);
    pthread_join(thr_exec,   NULL);
    pthread_join(thr_pub,    NULL);

    sem_close(sem_p1);
    munmap(shm, sizeof(SharedState));
    close(shm_fd);
    LOG("[P1] pacman_process terminado");
    return 0;
}