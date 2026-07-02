/*
 * time_tester.c — Benchmark de tiempo de ejecución del juego
 *
 * Uso: ./time_tester <map_dir> [max_ticks]
 * Ej:  ./time_tester maps/Caso1 50
 *
 * Redirige stdout del scheduler a /dev/null para que ncurses
 * no contamine la salida del benchmark. El tiempo se imprime
 * en una línea limpia parseable por benchmark.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <map_dir> [max_ticks]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *map_dir   = argv[1];
    char *max_ticks = (argc >= 3) ? argv[2] : "50";

    setenv("MAP_DIR", map_dir, 1);

    struct timespec t0, t1;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) < 0) {
        perror("clock_gettime start");
        return EXIT_FAILURE;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return EXIT_FAILURE; }

    if (pid == 0) {
        /* Hijo: redirigir stdout a /dev/null para suprimir ncurses */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        /* stderr también (logs de procesos ya van a pacman.log) */
        dup2(STDOUT_FILENO, STDERR_FILENO);

        setenv("PACMAN_TICK_MS", "0", 1); /* sin delay para benchmark */
        execlp("./scheduler_process", "./scheduler_process", map_dir, max_ticks, NULL);
        perror("execlp scheduler_process");
        exit(EXIT_FAILURE);
    }

    /* Padre: esperar fin */
    int status;
    waitpid(pid, &status, 0);

    if (clock_gettime(CLOCK_MONOTONIC, &t1) < 0) {
        perror("clock_gettime end");
        return EXIT_FAILURE;
    }

    long diff_sec  = t1.tv_sec  - t0.tv_sec;
    long diff_nsec = t1.tv_nsec - t0.tv_nsec;
    if (diff_nsec < 0) { diff_sec--; diff_nsec += 1000000000L; }

    double ms = diff_sec * 1000.0 + diff_nsec / 1000000.0;

    /* Salida en formato parseble: una sola línea con el número en punto flotante
     * El benchmark.sh busca la línea "TIME_MS:" para extraer el valor */
    printf("TIME_MS:%.4f\n", ms);
    fflush(stdout);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
