#!/bin/bash
# benchmark.sh — Benchmark de rendimiento PAC-MAN Concurrente
# Uso: ./benchmark.sh [iteraciones] [ticks]
# Ej:  ./benchmark.sh 1000 50

if [ ! -f "./time_tester" ]; then
    echo "[benchmark] Compilando time_tester..."
    make time_tester
fi

ITERACIONES=${1:-100}
TICKS=${2:-50}
CASOS=("Caso1" "Caso2" "Caso3")
CSV="resultados_benchmark.csv"

echo "=================================================="
echo "   BENCHMARK PAC-MAN CONCURRENTE"
echo "   Iteraciones: $ITERACIONES | Ticks: $TICKS"
echo "=================================================="

echo "Caso,Iteracion,Tiempo_ms" > "$CSV"

for CASO in "${CASOS[@]}"; do
    printf "\n---> %s  (ejecutando %d iteraciones...)\n" "$CASO" "$ITERACIONES"

    TIEMPOS=()
    VALIDOS=0
    ERRORES=0

    for ((i=1; i<=ITERACIONES; i++)); do
        rm -f /dev/shm/pacman_shm \
              /dev/shm/sem.pacman_sem_p1 \
              /dev/shm/sem.pacman_sem_p2 \
              pacman.log

        T=$(./time_tester "maps/$CASO" "$TICKS" 2>/dev/null \
            | grep '^TIME_MS:' | cut -d: -f2)

        if [ -z "$T" ]; then
            echo "$CASO,$i,ERROR" >> "$CSV"
            ERRORES=$((ERRORES + 1))
            continue
        fi

        echo "$CASO,$i,$T" >> "$CSV"
        TIEMPOS+=("$T")
        VALIDOS=$((VALIDOS + 1))

        if (( i % 100 == 0 )); then
            printf "    [%d/%d completadas]\n" "$i" "$ITERACIONES"
        fi
    done

    if [ "$VALIDOS" -eq 0 ]; then
        echo "  Sin resultados válidos para $CASO"
        continue
    fi

    STATS=$(printf '%s\n' "${TIEMPOS[@]}" | awk '
    BEGIN { n=0; sum=0; min=999999999; max=0; sum2=0 }
    {
        v = $1+0
        sum  += v
        sum2 += v*v
        if (v < min) min = v
        if (v > max) max = v
        n++
        vals[n] = v
    }
    END {
        media = sum / n
        varianza = (sum2 / n) - (media * media)
        desv = (varianza > 0) ? sqrt(varianza) : 0
        cv   = (media > 0) ? (desv / media * 100) : 0

        for (i=1; i<=n; i++) sorted[i] = vals[i]
        for (i=1; i<=n; i++)
            for (j=i+1; j<=n; j++)
                if (sorted[j] < sorted[i]) { tmp=sorted[i]; sorted[i]=sorted[j]; sorted[j]=tmp }

        p25_i = int(n*0.25)+1; p50_i = int(n*0.50)+1
        p75_i = int(n*0.75)+1; p95_i = int(n*0.95)+1

        printf "N=%d MEDIA=%.4f MIN=%.4f MAX=%.4f DESV=%.4f CV=%.4f RANGO=%.4f P25=%.4f P50=%.4f P75=%.4f P95=%.4f",
               n, media, min, max, desv, cv, (max-min),
               sorted[p25_i], sorted[p50_i], sorted[p75_i], sorted[p95_i]
    }')

    N=$(     echo "$STATS" | grep -oP 'N=\K[^ ]+')
    MEDIA=$( echo "$STATS" | grep -oP 'MEDIA=\K[^ ]+')
    MIN=$(   echo "$STATS" | grep -oP 'MIN=\K[^ ]+')
    MAX=$(   echo "$STATS" | grep -oP 'MAX=\K[^ ]+')
    DESV=$(  echo "$STATS" | grep -oP 'DESV=\K[^ ]+')
    CV=$(    echo "$STATS" | grep -oP 'CV=\K[^ ]+')
    RANGO=$( echo "$STATS" | grep -oP 'RANGO=\K[^ ]+')
    P25=$(   echo "$STATS" | grep -oP 'P25=\K[^ ]+')
    P50=$(   echo "$STATS" | grep -oP 'P50=\K[^ ]+')
    P75=$(   echo "$STATS" | grep -oP 'P75=\K[^ ]+')
    P95=$(   echo "$STATS" | grep -oP 'P95=\K[^ ]+')

    echo "$CASO,MEDIA,$MEDIA"       >> "$CSV"
    echo "$CASO,MIN,$MIN"           >> "$CSV"
    echo "$CASO,MAX,$MAX"           >> "$CSV"
    echo "$CASO,DESV_EST,$DESV"     >> "$CSV"
    echo "$CASO,P50_MEDIANA,$P50"   >> "$CSV"

    echo "  ┌─────────────────────────────────────────┐"
    printf "  │  %-10s  Resultados (%d válidas)       │\n" "$CASO" "$VALIDOS"
    echo "  ├──────────────────────┬──────────────────┤"
    printf "  │  Media              │  %10s ms    │\n" "$MEDIA"
    printf "  │  Mediana (P50)      │  %10s ms    │\n" "$P50"
    printf "  │  Desv. Estándar     │  %10s ms    │\n" "$DESV"
    printf "  │  Coef. Variación    │  %10s %%     │\n" "$CV"
    echo "  ├──────────────────────┼──────────────────┤"
    printf "  │  Mínimo             │  %10s ms    │\n" "$MIN"
    printf "  │  Máximo             │  %10s ms    │\n" "$MAX"
    printf "  │  Rango (max-min)    │  %10s ms    │\n" "$RANGO"
    echo "  ├──────────────────────┼──────────────────┤"
    printf "  │  Percentil 25       │  %10s ms    │\n" "$P25"
    printf "  │  Percentil 75       │  %10s ms    │\n" "$P75"
    printf "  │  Percentil 95       │  %10s ms    │\n" "$P95"
    echo "  ├──────────────────────┼──────────────────┤"
    printf "  │  Errores            │  %10d        │\n" "$ERRORES"
    echo "  └──────────────────────┴──────────────────┘"
done

echo ""
echo "  CSV guardado en: $CSV"
echo "=================================================="