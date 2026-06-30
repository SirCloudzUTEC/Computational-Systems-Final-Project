#!/bin/bash
# benchmark.sh — Corre 1000 iteraciones por caso y calcula los tiempos promedio.

# Asegurar que el ejecutable de pruebas esté compilado
if [ ! -f "./time_tester" ]; then
    echo "Compilando proyecto..."
    make time_tester
fi

ITERACIONES=1000
CASOS=("Caso1" "Caso2" "Caso3")

echo "=================================================="
echo "   INICIANDO BENCHMARK MASIVO (1000 ITERACIONES)   "
echo "=================================================="

echo "Caso,Iteracion,Tiempo_ms" > resultados_detallados.csv

for CASO in "${CASOS[@]}"; do
    echo "---> Procesando $CASO..."
    SUMA_TIEMPOS=0
    
    for (i=1; i<=ITERACIONES; i++)); do
        TIEMPO_MS=$(./time_tester "maps/$CASO" 300 | grep "Tiempo Equivalente:" | awk '{print $3}')
        
        if [ -z "$TIEMPO_MS" ]; then
            TIEMPO_MS=0
        fi
        
        echo "$CASO,$i,$TIEMPO_MS" >> resultados_detallados.csv
        
        SUMA_TIEMPOS=$(echo "$SUMA_TIEMPOS + $TIEMPO_MS" | bc)
    done
    
    # Calcular la media (promedio)
    MEDIA_MS=$(echo "scale=4; $SUMA_TIEMPOS / $ITERACIONES" | bc)
    
    echo " Finalizado $CASO. Tiempo Promedio (Media): $MEDIA_MS ms"
    echo "--------------------------------------------------"
done

echo "Analisis masivo completado! Los datos detallados se guardaron en 'resultados_detallados.csv'."
