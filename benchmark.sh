#!/bin/bash
# benchmark.sh — Mide el tiempo de ejecución promedio por caso.
# Uso: ./benchmark.sh [iteraciones] [ticks]
# Ej:  ./benchmark.sh 10 50

if [ ! -f "./time_tester" ]; then
    echo "[benchmark] Compilando time_tester..."
    make time_tester
fi

ITERACIONES=${1:-10}
TICKS=${2:-50}
CASOS=("Caso1" "Caso2" "Caso3")

echo "=================================================="
echo "   BENCHMARK PAC-MAN CONCURRENTE"
echo "   Iteraciones: $ITERACIONES | Ticks por corrida: $TICKS"
echo "=================================================="

CSV="resultados_benchmark.csv"
echo "Caso,Iteracion,Tiempo_ms" > "$CSV"

for CASO in "${CASOS[@]}"; do
    echo ""
    echo "---> Procesando $CASO..."
    SUMA="0"
    MIN=""
    MAX="0"
    VALIDOS=0

    for ((i=1; i<=ITERACIONES; i++)); do
        # Limpiar residuos entre corridas
        rm -f /dev/shm/pacman_shm \
              /dev/shm/sem.pacman_sem_p1 \
              /dev/shm/sem.pacman_sem_p2 \
              pacman.log

        # Ejecutar y capturar solo la línea TIME_MS
        TIEMPO_MS=$(./time_tester "maps/$CASO" "$TICKS" 2>/dev/null \
                    | grep '^TIME_MS:' | cut -d: -f2)

        if [ -z "$TIEMPO_MS" ]; then
            echo "  [!] Iter $i: sin resultado, saltando"
            echo "$CASO,$i,ERROR" >> "$CSV"
            continue
        fi

        echo "$CASO,$i,$TIEMPO_MS" >> "$CSV"
        SUMA=$(echo "$SUMA + $TIEMPO_MS" | bc)
        VALIDOS=$((VALIDOS + 1))

        # Min/Max
        if [ -z "$MIN" ]; then
            MIN="$TIEMPO_MS"
        else
            [ "$(echo "$TIEMPO_MS < $MIN" | bc)" = "1" ] && MIN="$TIEMPO_MS"
        fi
        [ "$(echo "$TIEMPO_MS > $MAX" | bc)" = "1" ] && MAX="$TIEMPO_MS"

        printf "  Iter %3d/%d: %s ms\n" "$i" "$ITERACIONES" "$TIEMPO_MS"
    done

    if [ "$VALIDOS" -gt 0 ]; then
        MEDIA=$(echo "scale=4; $SUMA / $VALIDOS" | bc)
        echo "  ─────────────────────────────────────"
        echo "  Válidas : $VALIDOS / $ITERACIONES"
        echo "  Media   : $MEDIA ms"
        echo "  Mínimo  : $MIN ms"
        echo "  Máximo  : $MAX ms"
        echo "  ─────────────────────────────────────"
    else
        echo "  Sin resultados válidos para $CASO"
    fi
done

echo ""
echo "Resultados guardados en: $CSV"
echo "=================================================="
