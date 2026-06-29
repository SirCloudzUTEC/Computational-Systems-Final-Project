#!/bin/bash
# run.sh — menú para elegir caso de prueba y ejecutar el juego
echo "=== Pac-Man Concurrente — Selección de caso ==="
echo "1) Caso1"
echo "2) Caso2"
echo "3) Caso3"
read -p "Elige un caso [1-3]: " opt

case $opt in
  1) DIR="maps/Caso1" ;;
  2) DIR="maps/Caso2" ;;
  3) DIR="maps/Caso3" ;;
  *) echo "Opción inválida"; exit 1 ;;
esac

read -p "Ticks máximos [default 300]: " ticks
ticks=${ticks:-300}

# limpiar SHM/semáforos residuales de una corrida anterior que no haya cerrado bien
rm -f /dev/shm/pacman_shm /dev/shm/sem.pacman_sem_p1 /dev/shm/sem.pacman_sem_p2

./scheduler_process "$DIR" "$ticks"
