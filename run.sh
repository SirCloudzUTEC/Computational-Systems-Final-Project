#!/bin/bash
# run.sh — Lanzador del menú interactivo de PAC-MAN Concurrente
# Usar desde el directorio FinalProject/

set -e

if [ ! -f "./scheduler_process" ] || [ ! -f "./menu" ]; then
    echo "[run.sh] Compilando..."
    make all
fi

rm -f /dev/shm/pacman_shm \
      /dev/shm/sem.pacman_sem_p1 \
      /dev/shm/sem.pacman_sem_p2

./menu
