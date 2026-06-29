#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <ncurses.h>
#include <signal.h>
#include <errno.h>

#include "shared.h"
#include "utils.h"

#define COL_WALL      1
#define COL_PATH      2
#define COL_PELLET    3
#define COL_PACMAN    4
#define COL_GHOST0    5
#define COL_GHOST1    6
#define COL_GHOST2    7
#define COL_GHOST3    8
#define COL_HUD       9
#define COL_POWER    10
#define COL_GAMEOVER 11

static SharedState *shm    = NULL;
static int          shm_fd = -1;
static int ncurses_active  = 0;

static int prev_pac_x = -1;
static int prev_pac_y = -1;
static int last_dir   = 0;  

static const char *pac_sym(int dir, int powered) {
    if (powered) { (void)dir; return "@"; }
    const char *s[] = { ">", "<", "^", "v" };
    return s[dir];
}

static const char *ghost_sym[NUM_GHOSTS] = { "A", "B", "C", "D" };

static void cleanup_ncurses(void) {
    if (ncurses_active) {
        endwin();
        ncurses_active = 0;
    }
}

static void sig_handler(int sig) {
    (void)sig;
    cleanup_ncurses();
    _exit(0);
}

static void init_ncurses(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    initscr();
    ncurses_active = 1;
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COL_WALL,      COLOR_BLUE,    COLOR_BLUE);
        init_pair(COL_PATH,      COLOR_BLACK,   -1);
        init_pair(COL_PELLET,    COLOR_WHITE,   -1);
        init_pair(COL_PACMAN,    COLOR_YELLOW,  -1);
        init_pair(COL_GHOST0,    COLOR_RED,     -1);
        init_pair(COL_GHOST1,    COLOR_CYAN,    -1);
        init_pair(COL_GHOST2,    COLOR_MAGENTA, -1);
        init_pair(COL_GHOST3,    COLOR_GREEN,   -1);
        init_pair(COL_HUD,       COLOR_WHITE,   -1);
        init_pair(COL_POWER,     COLOR_YELLOW,  COLOR_BLACK);
        init_pair(COL_GAMEOVER,  COLOR_RED,     COLOR_BLACK);
    }
}

static void render_frame(void) {
    clear();

    int rows = shm->map_rows;
    int cols = shm->map_cols;

    pthread_mutex_lock(&shm->mutex_pacman_pos);
    int pac_x = shm->pacman_x;
    int pac_y = shm->pacman_y;
    int score = shm->pacman_score;
    int lives = shm->pacman_lives;
    pthread_mutex_unlock(&shm->mutex_pacman_pos);

    pthread_mutex_lock(&shm->mutex_power);
    int power = shm->pacman_power;
    pthread_mutex_unlock(&shm->mutex_power);

    int tick    = shm->global_tick;
    int prio_p  = shm->prioridad_pacman;
    int prio_e  = shm->prioridad_enemy;
    int game_ov = shm->game_over;

    if (prev_pac_x >= 0) {
        if      (pac_x > prev_pac_x) last_dir = 0;
        else if (pac_x < prev_pac_x) last_dir = 1;
        else if (pac_y < prev_pac_y) last_dir = 2;
        else if (pac_y > prev_pac_y) last_dir = 3;
    }
    prev_pac_x = pac_x;
    prev_pac_y = pac_y;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            char cell = shm->map_grid[r][c];
            int is_pac   = (r == pac_y && c == pac_x);
            int ghost_id = -1;

            pthread_mutex_lock(&shm->mutex_ghost_pos);
            for (int g = 0; g < NUM_GHOSTS; g++) {
                if (shm->ghost_active[g] &&
                    shm->ghost_x[g] == c && shm->ghost_y[g] == r) {
                    ghost_id = g;
                    break;
                }
            }
            pthread_mutex_unlock(&shm->mutex_ghost_pos);

            if (is_pac) {
                attron(COLOR_PAIR(COL_PACMAN) | A_BOLD);
                mvprintw(r + 2, c * 2, "%s", pac_sym(last_dir, power > 0));
                attroff(COLOR_PAIR(COL_PACMAN) | A_BOLD);
                mvprintw(r + 2, c * 2 + 1, " ");
            } else if (ghost_id >= 0) {
                int cp = COL_GHOST0 + ghost_id;
                if (power > 0) cp = COL_POWER;
                attron(COLOR_PAIR(cp) | A_BOLD);
                mvprintw(r + 2, c * 2, "%s", ghost_sym[ghost_id]);
                attroff(COLOR_PAIR(cp) | A_BOLD);
                mvprintw(r + 2, c * 2 + 1, " ");
            } else if (cell == CELL_WALL) {
                attron(COLOR_PAIR(COL_WALL));
                mvprintw(r + 2, c * 2, "##");
                attroff(COLOR_PAIR(COL_WALL));
            } else if (cell == CELL_PELLET) {
                attron(COLOR_PAIR(COL_PELLET) | A_BOLD);
                mvprintw(r + 2, c * 2, "* ");
                attroff(COLOR_PAIR(COL_PELLET) | A_BOLD);
            } else {
                mvprintw(r + 2, c * 2, "  ");
            }
        }
    }

    attron(COLOR_PAIR(COL_HUD) | A_BOLD);
    mvprintw(0, 0, " PAC-MAN CONCURRENTE");
    mvprintw(1, 0, " Tick:%-4d  Score:%-6d  Vidas:%d  Prio[P1:%d P2:%d]",
             tick, score, lives, prio_p, prio_e);
    attroff(COLOR_PAIR(COL_HUD) | A_BOLD);

    if (power > 0) {
        attron(COLOR_PAIR(COL_POWER) | A_BOLD);
        mvprintw(1, cols * 2 - 16, " ** POWER: %2d ** ", power);
        attroff(COLOR_PAIR(COL_POWER) | A_BOLD);
    }

    int bot = rows + 3;
    attron(COLOR_PAIR(COL_HUD));
    mvprintw(bot, 0, " Fantasmas: ");
    for (int g = 0; g < NUM_GHOSTS; g++) {
        int cp = (power > 0) ? COL_POWER : (COL_GHOST0 + g);
        attron(COLOR_PAIR(cp) | A_BOLD);
        int gx, gy;
        pthread_mutex_lock(&shm->mutex_ghost_pos);
        gx = shm->ghost_x[g]; gy = shm->ghost_y[g];
        pthread_mutex_unlock(&shm->mutex_ghost_pos);
        if (shm->ghost_active[g])
            printw("[%s:(%d,%d)] ", ghost_sym[g], gx, gy);
        attroff(COLOR_PAIR(cp) | A_BOLD);
    }
    attroff(COLOR_PAIR(COL_HUD));

    if (game_ov) {
        int mid_r = rows / 2 + 2;
        int mid_c = (cols * 2 / 2) - 12;
        if (mid_c < 0) mid_c = 0;
        attron(COLOR_PAIR(COL_GAMEOVER) | A_BOLD);
        mvprintw(mid_r,     mid_c, "  +====================+  ");
        mvprintw(mid_r + 1, mid_c, "  |    GAME  OVER      |  ");
        mvprintw(mid_r + 2, mid_c, "  |  Score: %-8d  |  ", score);
        mvprintw(mid_r + 3, mid_c, "  +====================+  ");
        attroff(COLOR_PAIR(COL_GAMEOVER) | A_BOLD);
        attron(COLOR_PAIR(COL_HUD));
        mvprintw(mid_r + 5, mid_c, "  %s", shm->win_reason);
        attroff(COLOR_PAIR(COL_HUD));
    }

    refresh();
}

int main(void) {
    int retries = 30;
    while (retries-- > 0) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd >= 0) break;
        if (errno != ENOENT) { perror("[P3] shm_open"); return 1; }
        usleep(50000); /* 50ms */
    }
    if (shm_fd < 0) { fprintf(stderr, "[P3] No se pudo abrir SHM\n"); return 1; }

    shm = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("[P3] mmap"); close(shm_fd); return 1; }

    init_ncurses();

    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        int r = sem_timedwait(&shm->sem_render_ready, &ts);
        if (r < 0 && errno == ETIMEDOUT) {
            if (shm->game_over) break;
            continue;
        }
        render_frame();
        if (shm->game_over) {
            render_frame();
            ms_sleep(3000);
            break;
        }
    }

    cleanup_ncurses();
    munmap(shm, sizeof(SharedState));
    close(shm_fd);
    return 0;
}
