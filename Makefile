# ─────────────────────────────────────────────
# Makefile — PAC-MAN concurrente (procesos+hilos)
# ─────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g \
           -I./include \
           -D_GNU_SOURCE
LDFLAGS = -lpthread -lrt -lncurses -ltinfo

SRC     = src
BIN     = .

TARGETS = scheduler_process \
          pacman_process     \
          enemy_process      \
          renderer_process   \
          menu

.PHONY: all clean run

all: $(TARGETS)

scheduler_process: $(SRC)/scheduler_process.c include/shared.h include/utils.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

pacman_process: $(SRC)/pacman_process.c include/shared.h include/utils.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

enemy_process: $(SRC)/enemy_process.c include/shared.h include/utils.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

renderer_process: $(SRC)/renderer_process.c include/shared.h include/utils.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Lanzar menú interactivo
run: all
	./menu

menu: $(SRC)/menu.c
	$(CC) -Wall -Wextra -O2 -g -o $@ $<

clean:
	rm -f $(TARGETS) menu
	# limpiar SHM y semáforos POSIX residuales
	-rm -f /dev/shm/pacman_shm
	-rm -f /dev/shm/sem.pacman_sem_p1
	-rm -f /dev/shm/sem.pacman_sem_p2
