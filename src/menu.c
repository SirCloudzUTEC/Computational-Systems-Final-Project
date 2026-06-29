/*
 * menu.c — Menú interactivo para PAC-MAN Concurrente
 *
 * Permite elegir caso, configurar ticks y relanzar el juego
 * en un loop hasta que el usuario elija Salir.
 *
 * Compilar: gcc -o menu src/menu.c  (sin flags extra)
 * Usar:     ./menu   (desde el directorio FinalProject/)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

/* ── ANSI colors ── */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_RED     "\033[31m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_WHITE   "\033[37m"
#define ANSI_MAGENTA "\033[35m"

/* Rutas relativas a FinalProject/ */
static const char *CASES[]     = { "maps/Caso1", "maps/Caso2", "maps/Caso3" };
static const char *CASE_NAMES[] = { "Caso 1 — Básico",
                                    "Caso 2 — SET_PRIORITY",
                                    "Caso 3 — Colisión / Power Pellet" };
#define NUM_CASES 3
#define DEFAULT_TICKS 300

/* Semáforos y SHM que pueden quedar residuales */
static const char *RESIDUAL_FILES[] = {
    "/dev/shm/pacman_shm",
    "/dev/shm/sem.pacman_sem_p1",
    "/dev/shm/sem.pacman_sem_p2",
    NULL
};

/* ────────────────────────────────────────────────── */

static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void print_banner(void) {
    printf(ANSI_YELLOW ANSI_BOLD);
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║        PAC-MAN CONCURRENTE EN C          ║\n");
    printf("  ║      IS2021 — Computing Systems          ║\n");
    printf("  ╚══════════════════════════════════════════╝\n");
    printf(ANSI_RESET "\n");
}

static void cleanup_residuals(void) {
    for (int i = 0; RESIDUAL_FILES[i]; i++) {
        if (remove(RESIDUAL_FILES[i]) == 0) {
            printf(ANSI_CYAN "  [limpieza] Eliminado: %s\n" ANSI_RESET,
                   RESIDUAL_FILES[i]);
        }
    }
}

static int check_binaries(void) {
    const char *bins[] = { "./scheduler_process", "./pacman_process",
                           "./enemy_process",     "./renderer_process", NULL };
    int ok = 1;
    for (int i = 0; bins[i]; i++) {
        struct stat st;
        if (stat(bins[i], &st) != 0) {
            printf(ANSI_RED "  [ERROR] Falta ejecutable: %s\n" ANSI_RESET, bins[i]);
            ok = 0;
        }
    }
    return ok;
}

static void print_case_info(int caso_idx) {
    printf(ANSI_BLUE "  ── Descripción del caso ──\n" ANSI_RESET);
    switch (caso_idx) {
        case 0:
            printf("  Caso 1: movimientos simples, prioridades fijas\n");
            printf("  Pac-Man: UP x18 | Fantasmas: UP varios\n");
            break;
        case 1:
            printf("  Caso 2: incluye instrucción SET_PRIORITY\n");
            printf("  Se prueba el cambio dinámico de prioridad via buzón en SHM\n");
            break;
        case 2:
            printf("  Caso 3: Power Pellet presente en el mapa\n");
            printf("  Se prueban colisiones con y sin power activo\n");
            break;
    }
    printf("\n");
}

/* Lee una línea de stdin, devuelve entero o default_val si vacía/inválida */
static int read_int_default(int default_val) {
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return default_val;
    /* eliminar newline */
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '\0') return default_val;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') return default_val;
    return (int)v;
}

static void run_case(int caso_idx, int max_ticks) {
    const char *map_dir = CASES[caso_idx];

    printf("\n" ANSI_GREEN ANSI_BOLD);
    printf("  ══════════════════════════════════════════\n");
    printf("  Iniciando %s  (max_ticks=%d)\n", CASE_NAMES[caso_idx], max_ticks);
    printf("  ══════════════════════════════════════════\n");
    printf(ANSI_RESET "\n");
    fflush(stdout);

    /* Limpiar residuos de corridas anteriores y log previo */
    cleanup_residuals();
    remove("pacman.log"); /* log fresco por cada corrida */

    sleep(1); /* pausa breve para que el usuario vea el mensaje */

    /* Ejecutar scheduler_process (que a su vez hace fork de P1/P2/P3) */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        /* Proceso hijo: exec scheduler */
        char ticks_str[32];
        snprintf(ticks_str, sizeof(ticks_str), "%d", max_ticks);
        execlp("./scheduler_process", "scheduler_process",
               map_dir, ticks_str, NULL);
        perror("execlp scheduler_process");
        _exit(1);
    }

    /* Proceso padre: esperar a que el scheduler (y todos sus hijos) terminen */
    int status;
    waitpid(pid, &status, 0);

    /* Limpieza post-ejecución */
    cleanup_residuals();

    printf("\n" ANSI_CYAN ANSI_BOLD);
    printf("  ══════════════════════════════════════════\n");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("  Caso finalizado correctamente.\n");
    } else {
        printf("  El proceso terminó con estado: %d\n", WEXITSTATUS(status));
    }
    printf("  ══════════════════════════════════════════\n");
    printf(ANSI_RESET "\n");

    printf("  Presiona ENTER para volver al menú...");
    fflush(stdout);
    /* consumir cualquier input residual */
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}


int main(void) {
    /* Verificar que estamos en el directorio correcto */
    if (!check_binaries()) {
        printf(ANSI_RED "\n  Asegúrate de ejecutar desde FinalProject/ después de 'make'.\n" ANSI_RESET);
        return 1;
    }

    int max_ticks = DEFAULT_TICKS;
    int running   = 1;

    while (running) {
        clear_screen();
        print_banner();

        /* Mostrar menú con ticks actuales */
        printf(ANSI_WHITE ANSI_BOLD "  Selecciona una opción:\n" ANSI_RESET "\n");
        for (int i = 0; i < NUM_CASES; i++) {
            printf("    " ANSI_CYAN "[%d]" ANSI_RESET "  %s\n", i + 1, CASE_NAMES[i]);
        }
        printf("\n");
        printf("    " ANSI_MAGENTA "[4]" ANSI_RESET "  Ajustar ticks máximos (actual: %d)\n", max_ticks);
        printf("    " ANSI_RED    "[0]" ANSI_RESET "  Salir\n");
        printf("\n");
        printf("  Opción: ");
        fflush(stdout);

        int opt = read_int_default(-1);

        switch (opt) {
            case 1:
            case 2:
            case 3: {
                int idx = opt - 1;
                clear_screen();
                print_banner();
                print_case_info(idx);

                printf("  Ticks máximos [ENTER = %d]: ", max_ticks);
                fflush(stdout);
                int t = read_int_default(max_ticks);
                if (t < 10 || t > 9999) {
                    printf(ANSI_RED "  Valor fuera de rango (10-9999), usando %d\n" ANSI_RESET, max_ticks);
                    t = max_ticks;
                }
                run_case(idx, t);
                break;
            }
            case 4: {
                printf("\n  Nuevo valor de ticks máximos [10-9999]: ");
                fflush(stdout);
                int t = read_int_default(max_ticks);
                if (t >= 10 && t <= 9999) {
                    max_ticks = t;
                    printf(ANSI_GREEN "  Ticks máximos actualizado a %d\n" ANSI_RESET, max_ticks);
                } else {
                    printf(ANSI_RED "  Valor inválido, manteniendo %d\n" ANSI_RESET, max_ticks);
                }
                sleep(1);
                break;
            }
            case 0:
                running = 0;
                break;
            default:
                printf(ANSI_RED "  Opción inválida.\n" ANSI_RESET);
                sleep(1);
                break;
        }
    }

    clear_screen();
    printf(ANSI_YELLOW ANSI_BOLD "\n  ¡Hasta luego! — PAC-MAN Concurrente\n\n" ANSI_RESET);
    return 0;
}
