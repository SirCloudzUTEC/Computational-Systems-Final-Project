#ifndef SHARED_H
#define SHARED_H

#include <semaphore.h>
#include <pthread.h>
#include <stdint.h>

#define MAX_X        30
#define MAX_Y        20
#define NUM_GHOSTS    4

#define CELL_WALL       'X'
#define CELL_PATH       'O'
#define CELL_PELLET     '*'
#define CELL_PACMAN     'P'
#define CELL_GHOST_A    'A'
#define CELL_GHOST_B    'B'
#define CELL_GHOST_C    'C'
#define CELL_GHOST_D    'D'

#define DEFAULT_MAX_TICKS 300

#define SHM_NAME   "/pacman_shm"
#define SEM_PACMAN "/pacman_sem_p1"
#define SEM_ENEMY  "/pacman_sem_p2"
#define SEM_RENDER "/pacman_sem_p3"

#define POWER_DURATION 10   


typedef struct {
    int  global_tick;
    int  max_ticks;
    int  game_over;         
    char win_reason[64];    


    int  pacman_x;
    int  pacman_y;
    int  pacman_score;
    int  pacman_lives;
    int  pacman_power;       


    int  collision_detected;
    int  collision_tick;
    int  collision_ghost_id;


    int  prioridad_pacman;
    int  prioridad_enemy;

  
    int  pending_priority_pacman;
    int  priority_request_active;
    int  pending_priority_enemy;
    int  enemy_priority_request_active;


    char map_grid[MAX_Y][MAX_X];
    int  map_rows;
    int  map_cols;


    int  ghost_x[NUM_GHOSTS];
    int  ghost_y[NUM_GHOSTS];
    int  ghost_active[NUM_GHOSTS];


    pthread_mutex_t mutex_pacman_pos;  
    pthread_mutex_t mutex_collision;    
    pthread_mutex_t mutex_priority;    
    pthread_mutex_t mutex_ghost_pos;    
    pthread_mutex_t mutex_map;          
    pthread_mutex_t mutex_power;        


    sem_t sem_render_ready;

} SharedState;

#endif 
