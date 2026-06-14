#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include "mining.h"

#define QUEUE_SIZE 12

typedef struct
{
    void *buffer[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} work_queue;

void queue_init(work_queue *queue);
void queue_enqueue(work_queue *queue, void *new_work);
void ASIC_jobs_queue_clear(work_queue *queue);
void ASIC_jobs_queue_clear_pool(work_queue *queue, int pool_id);
void *queue_dequeue(work_queue *queue);
bool queue_dequeue_if_full(work_queue *queue, void **work);
int queue_count(work_queue *queue);
void queue_clear(work_queue *queue);

#endif // WORK_QUEUE_H
