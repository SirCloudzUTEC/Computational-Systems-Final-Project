# PAC-MAN Concurrente en C
### Arquitectura híbrida: procesos POSIX + pthreads + memoria compartida

---

## Estructura del proyecto

```
pacman/
├── Makefile
├── include/
│   ├── shared.h        ← Memoria compartida, constantes, estructura SharedState
│   └── utils.h         ← Macros de logging, sleep, dirección, is_walkable
├── src/
│   ├── scheduler_process.c   ← P0: scheduler, ticks, prioridades, game_over
│   ├── pacman_process.c      ← P1: lector de movimientos, executor, publisher
│   ├── enemy_process.c       ← P2: controller + 4 ghost threads + collision
│   └── renderer_process.c    ← P3: ncurses visual (laberinto + HUD)
└── maps/
    ├── map.txt               ← Laberinto (editable)
    ├── pacman_moves.txt      ← Instrucciones de Pac-Man
    ├── ghost_1_moves.txt     ← Instrucciones fantasma 1
    ├── ghost_2_moves.txt     ← Instrucciones fantasma 2
    ├── ghost_3_moves.txt     ← Instrucciones fantasma 3
    └── ghost_4_moves.txt     ← Instrucciones fantasma 4
```

---

## Compilar

```bash
make          # compila los 4 binarios
make clean    # limpia binarios y residuos POSIX
```

Dependencias requeridas (Ubuntu/Debian):
```bash
sudo apt install gcc libncurses-dev
```

---

## Ejecutar

```bash
# Desde el directorio raíz del proyecto
make run
# o equivalentemente:
./scheduler_process maps/map.txt 300
```

P0 hace `fork()` + `execlp()` de P1, P2 y P3 automáticamente.
Los binarios `pacman_process`, `enemy_process` y `renderer_process`
deben estar en el mismo directorio que `scheduler_process`.

---

## Arquitectura de procesos e hilos

### P0 — `scheduler_process`
| Hilo | Función |
|------|---------|
| `tick_thread` | Incrementa `global_tick` cada `TICK_DELAY_MS` ms (atómico) |
| `scheduler_thread` | Lee tick, procesa prioridades/colisiones, decide ganador |
| `signal_thread` | Hace `sem_post` al semáforo del proceso ganador |

**Scheduler de prioridades:**
- Proceso con mayor `prioridad_X` gana el tick.
- Empate → Round Robin alternado.
- Al inicio de cada tick revisa `priority_request_active` y `enemy_priority_request_active`.

### P1 — `pacman_process`
| Hilo | Función |
|------|---------|
| `movement_reader_thread` | Lee `pacman_moves.txt`, inserta en cola interna |
| `movement_executor_thread` | Espera `sem_pacman`, consume instrucción, mueve Pac-Man |
| `pacman_publisher_thread` | Publica `(x, y, score)` en SHM bajo mutex |

### P2 — `enemy_process`
| Hilo | Función |
|------|---------|
| `enemy_controller_thread` | Espera `sem_enemy`, despierta los 4 ghost threads |
| `ghost_thread_[0..3]` | Cada uno lee su archivo, mueve su fantasma |
| `pacman_tracker_thread` | Copia posición de Pac-Man desde SHM cada 20 ms |
| `collision_thread` | Compara posiciones cada 15 ms, publica `collision_detected` |

### P3 — `renderer_process`
- Se bloquea en `sem_render_ready` (semáforo embebido en SHM, señalizado por P0).
- Renderiza en ncurses: laberinto con colores, sprites animados, HUD.
- Overlay de Game Over con resultado final.

---

## Sincronización

| Recurso | Mecanismo |
|---------|-----------|
| `pacman_x/y/score` | `mutex_pacman_pos` (process-shared) |
| `collision_detected/tick/ghost_id` | `mutex_collision` |
| `pending_priority_*` | `mutex_priority` |
| `ghost_x[]/ghost_y[]` (SHM) | `mutex_ghost_pos` |
| `map_grid[][]` | `mutex_map` |
| `pacman_power` | `mutex_power` |
| Cola interna P1 | `pthread_mutex` + `sem_t avail` |
| Ghost threads interno P2 | `sem_ghost_go[i]` + `sem_ghost_done[i]` |
| Señal P0→P1 turno | `sem_pacman` (semáforo POSIX nombrado) |
| Señal P0→P2 turno | `sem_enemy` (semáforo POSIX nombrado) |
| Señal P0→P3 render | `sem_render_ready` (sem_t embebido en SHM) |

---

## Archivos de movimientos

Formato (un token por línea):
```
UP
DOWN
LEFT
RIGHT
SET_PRIORITY <1-100>
```

- Movimiento inválido (pared o fuera de mapa) → **consume el turno**, el personaje no se mueve.
- `SET_PRIORITY` no modifica la prioridad directamente; escribe en el buzón de SHM y P0 la aplica al inicio del siguiente tick.

---

## Mapa (`map.txt`)

| Símbolo | Significado |
|---------|-------------|
| `X` | Pared (no transitable) |
| `O` | Camino libre (+10 pts al pisar) |
| `*` | Power pellet (+50 pts, activa modo poder por 10 ticks) |
| `P` | Posición inicial Pac-Man |
| `A` `B` `C` `D` | Posición inicial fantasma 1-4 |

Durante la inicialización P0 extrae las posiciones iniciales y reemplaza esos símbolos por `O`.

---

## Power Pellet

Cuando Pac-Man pisa `*`:
- Score +50.
- `pacman_power` se establece en `POWER_DURATION` (10 ticks).
- P0 decrementa `pacman_power` cada tick.
- Mientras `pacman_power > 0`, P0 ignora colisiones reportadas por P2.
- En P3 los fantasmas se renderizan con color especial (amarillo).

---

## Condiciones de finalización

| Condición | Quién la detecta |
|-----------|-----------------|
| `global_tick >= max_ticks` | P0 (`scheduler_thread`) |
| `pacman_lives <= 0` | P0 (tras procesar colisión de P2) |
| Sin más instrucciones en `pacman_moves.txt` | P1 (`movement_executor_thread`) |

Al activarse `game_over`, P0 hace `sem_post` en todos los semáforos para desbloquear P1, P2 y P3.

---

## Race conditions mitigadas

| Condición | Solución |
|-----------|----------|
| `tick_thread` vs `scheduler_thread` leen/escriben `global_tick` | `__atomic_fetch_add` / `__atomic_load_n` |
| `ghost_thread_i` actualiza `ghost_local_x[]` mientras `collision_thread` lee | `mtx_ghost_local` |
| `pacman_tracker_thread` actualiza `pac_last_x/y` mientras `collision_thread` lee | `mtx_pac_local` |
| `movement_reader_thread` inserta en cola mientras `movement_executor_thread` consume | `pthread_mutex` + semáforo de disponibilidad |
| `movement_executor_thread` actualiza posición mientras `pacman_publisher_thread` publica | `pub_mtx` + `pub_sem` |
| Publicación de prioridad desde P1/P2 mientras P0 la lee | `mutex_priority` (process-shared) |
| `collision_detected` escrito por P2 y leído por P0 | `mutex_collision` (process-shared) |

---

## Parámetros configurables (`include/shared.h` / `utils.h`)

| Constante | Default | Descripción |
|-----------|---------|-------------|
| `MAX_X` | 30 | Ancho máximo del mapa |
| `MAX_Y` | 20 | Alto máximo del mapa |
| `DEFAULT_MAX_TICKS` | 300 | Ticks por partida |
| `POWER_DURATION` | 10 | Duración del power-pellet en ticks |
| `TICK_DELAY_MS` | 120 | Milisegundos entre ticks |
