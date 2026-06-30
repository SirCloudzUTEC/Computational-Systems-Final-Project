#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    char *map_dir = "maps";
    char *max_ticks = "300";

    // Permitir pasar parámetros alternativos por consola
    if (argc >= 2) map_dir = argv[1];
    if (argc >= 3) max_ticks = argv[2];

    printf("   EJECUTABLE DE TESTEO DE TIEMPOS (BENCHMARK)    \n");
    printf("Configuración: %s | Max Ticks: %s\n\n", map_dir, max_ticks);

    // Configurar la variable de entorno necesaria para los procesos
    setenv("MAP_DIR", map_dir, 1);

    struct timespec start_time, end_time;
    
    // 1. Captura del tiempo inicial de alta resolución
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) == -1) {
        perror("Error al obtener clock_gettime (start)");
        return EXIT_FAILURE;
    }

    // 2. Crear proceso hijo para ejecutar el planificador central (P0)
    pid_t pid = fork();

    if (pid < 0) {
        perror("Error en fork()");
        return EXIT_FAILURE;
    } 
    else if (pid == 0) {
        // Redirigir stdout a /dev/null si no deseas ver prints repetitivos en la prueba
        // execlp("./scheduler_process", "./scheduler_process", map_dir, max_ticks, NULL);
        
        // Ejecución normal heredando logs a pacman.log
        execlp("./scheduler_process", "./scheduler_process", map_dir, max_ticks, NULL);
        
        // Si llega aquí, hubo un error al ejecutar el binario
        perror("Error en execlp de scheduler_process");
        exit(EXIT_FAILURE);
    } 
    else {
        // Proceso Padre: Esperar a que toda la simulación de procesos termine
        int status;
        waitpid(pid, &status, 0);

        // 3. Captura del tiempo final
        if (clock_gettime(CLOCK_MONOTONIC, &end_time) == -1) {
            perror("Error al obtener clock_gettime (end)");
            return EXIT_FAILURE;
        }

        // 4. Cálculos de diferencia de tiempo
        long diff_sec = end_time.tv_sec - start_time.tv_sec;
        long diff_nsec = end_time.tv_nsec - start_time.tv_nsec;
        
        if (diff_nsec < 0) {
            diff_sec -= 1;
            diff_nsec += 1000000000L;
        }

        double total_ms = (diff_sec * 1000.0) + (diff_nsec / 1000000.0);

        printf("RESULTADOS DEL TEST DE RENDIMIENTO:\n");
        printf("  - Tiempo Total Real: %ld seg, %ld ns\n", diff_sec, diff_nsec);
        printf("  - Tiempo Equivalente: %.4f milisegundos (ms)\n", total_ms);
        
        if (WIFEXITED(status)) {
            printf("  - Estado de salida de P0: %d\n", WEXITSTATUS(status));
        }
        printf("==================================================\n");
    }

    return EXIT_SUCCESS;
}
