/*
 * renderer_process.c  (P3)
 * ─────────────────────────
 * Renderiza en terminal usando ncurses:
 *   - Laberinto con colores
 *   - Pac-Man animado (C > ) < v ^)
 *   - Fantasmas con colores
 *   - HUD: tick, score, vidas, power-pellet, prioridades
 *
 * Se bloquea en sem_render_ready (señalizado por P0 en cada tick).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <ncurses.h>
#include <signal.h>

#include "shared.h"
#include "utils.h"

/* ── Pares de colores ncurses ── */
#define COL_WALL      1   /* azul sobre negro  */
#define COL_PATH      2   /* negro sobre negro  */
#define COL_PELLET    3   /* blanco sobre negro */
#define COL_PACMAN    4   /* amarillo sobre negro */
#define COL_GHOST0    5   /* rojo */
#define COL_GHOST1    6   /* cian */
#define COL_GHOST2    7   /* magenta */
#define COL_GHOST3    8   /* verde */
#define COL_HUD       9   /* blanco brillante */
#define COL_POWER    10   /* amarillo sobre negro (power activo) */
#define COL_GAMEOVER 11   /* rojo brillante */

static SharedState *shm    = NULL;
static int          shm_fd = -1;

/* ── Última dirección de Pac-Man (para sprite) ── */
static int prev_pac_x = -1;
static int last_dir   = 0;  /* 0=R 1=L 2=U 3=D */

/* símbolos según dirección */
static const char *pac_sym(int dir, int powered) {
    if (powered) {
        /* fantasmas asustados: Pac-Man "superpoderoso" */
        (void)dir;
        return "@";
    }
    const char *s[] = { ">", "<", "^", "v" };
    return s[dir];
}

static const char *ghost_sym[NUM_GHOSTS] = { "A", "B", "C", "D" };

/* ─────────────────────────────────────────────────────────
   Inicializar ncurses
   ───────────────────────────────────────────────────────── */
static void init_ncurses(void) {
    initscr();
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

/* ─────────────────────────────────────────────────────────
   Renderizar frame
   ───────────────────────────────────────────────────────── */
static void render_frame(void) {
    clear();

    int rows = shm->map_rows;
    int cols = shm->map_cols;

    /* ── Leer estado (bajo mutex mínimo) ── */
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

    /* actualizar dirección de Pac-Man */
    if (prev_pac_x >= 0) {
        if      (pac_x > prev_pac_x) last_dir = 0; /* R */
        else if (pac_x < prev_pac_x) last_dir = 1; /* L */
        else if (pac_y < prev_pac_x) last_dir = 2; /* U — comparación dummy; */
    }
    /* dirección simple basada en último desplazamiento */
    prev_pac_x = pac_x;

    /* ── Mapa ── */
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
                mvprintw(r + 1, c * 2, "%s", pac_sym(last_dir, power > 0));
                attroff(COLOR_PAIR(COL_PACMAN) | A_BOLD);
                mvprintw(r + 1, c * 2 + 1, " ");
            } else if (ghost_id >= 0) {
                int cp = COL_GHOST0 + ghost_id;
                if (power > 0) cp = COL_POWER;  /* fantasmas "asustados" */
                attron(COLOR_PAIR(cp) | A_BOLD);
                mvprintw(r + 1, c * 2, "%s", ghost_sym[ghost_id]);
                attroff(COLOR_PAIR(cp) | A_BOLD);
                mvprintw(r + 1, c * 2 + 1, " ");
            } else if (cell == CELL_WALL) {
                attron(COLOR_PAIR(COL_WALL));
                mvprintw(r + 1, c * 2, "██");
                attroff(COLOR_PAIR(COL_WALL));
            } else if (cell == CELL_PELLET) {
                attron(COLOR_PAIR(COL_PELLET) | A_BOLD);
                mvprintw(r + 1, c * 2, "* ");
                attroff(COLOR_PAIR(COL_PELLET) | A_BOLD);
            } else {
                mvprintw(r + 1, c * 2, "  ");
            }
        }
    }

    /* ── HUD superior ── */
    attron(COLOR_PAIR(COL_HUD) | A_BOLD);
    mvprintw(0, 0, " PAC-MAN | Tick: %-4d | Score: %-6d | Vidas: %d | "
             "Prio P1:%d P2:%d",
             tick, score, lives, prio_p, prio_e);
    attroff(COLOR_PAIR(COL_HUD) | A_BOLD);

    /* ── Power-pellet status ── */
    if (power > 0) {
        attron(COLOR_PAIR(COL_POWER) | A_BLINK | A_BOLD);
        mvprintw(0, cols * 2 - 20, " *** POWER %2d *** ", power);
        attroff(COLOR_PAIR(COL_POWER) | A_BLINK | A_BOLD);
    }

    /* ── Leyenda inferior ── */
    int bot = rows + 2;
    attron(COLOR_PAIR(COL_HUD));
    mvprintw(bot, 0, " Fantasmas: ");
    for (int g = 0; g < NUM_GHOSTS; g++) {
        int cp = (power > 0) ? COL_POWER : (COL_GHOST0 + g);
        attron(COLOR_PAIR(cp) | A_BOLD);
        int gx, gy;
        pthread_mutex_lock(&shm->mutex_ghost_pos);
        gx = shm->ghost_x[g]; gy = shm->ghost_y[g];
        pthread_mutex_unlock(&shm->mutex_ghost_pos);
        printw("[%s:(%d,%d)] ", ghost_sym[g], gx, gy);
        attroff(COLOR_PAIR(cp) | A_BOLD);
    }
    attroff(COLOR_PAIR(COL_HUD));

    /* ── Game Over overlay ── */
    if (game_ov) {
        int mid_r = rows / 2;
        int mid_c = cols - 10;
        attron(COLOR_PAIR(COL_GAMEOVER) | A_BOLD | A_BLINK);
        mvprintw(mid_r,     mid_c, "  ╔══════════════════╗  ");
        mvprintw(mid_r + 1, mid_c, "  ║   GAME  OVER     ║  ");
        mvprintw(mid_r + 2, mid_c, "  ║  Score: %-8d ║  ", score);
        mvprintw(mid_r + 3, mid_c, "  ╚══════════════════╝  ");
        attroff(COLOR_PAIR(COL_GAMEOVER) | A_BOLD | A_BLINK);
        mvprintw(mid_r + 5, mid_c, "  %s", shm->win_reason);
    }

    refresh();
}

/* ─────────────────────────────────────────────────────────
   main de P3
   ───────────────────────────────────────────────────────── */
int main(void) {
    /* Abrir memoria compartida */
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) { perror("[P3] shm_open"); return 1; }
    shm = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("[P3] mmap"); return 1; }

    init_ncurses();

    /* Bucle de render: bloqueado en semáforo, dibuja en cada tick */
    while (1) {
        sem_wait(&shm->sem_render_ready);
        render_frame();
        if (shm->game_over) {
            /* mostrar pantalla final 2 segundos */
            render_frame();
            ms_sleep(2500);
            break;
        }
    }

    endwin();
    munmap(shm, sizeof(SharedState));
    close(shm_fd);
    return 0;
}
