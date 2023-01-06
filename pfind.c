#include <stdlib.h>
#include <stdio.h>
#include <threads.h>

mtx_t mutex;
cnd_t condition;
cnd_t is_all_threads_ready;
long number_of_ready_threads = 0;
long number_of_desired_threads = 20;

typedef struct queue {
    int* arr;
    size_t size;
    size_t capacity;
    size_t front;
    size_t rear;
    mtx_t lock;
    cnd_t cond;
} Queue;

int queue_init(Queue* q, size_t capacity) {
    q->arr = malloc(capacity * sizeof(int));
    if (q->arr == NULL) {
        return -1;
    }
//    Initialize starting variables for queue
    q->size = 0;
    q->capacity = capacity;
    q->front = 0;
    q->rear = 0;
//    Initialize mutex and cond for queue.
    mtx_init(&q->lock, mtx_plain);
    cnd_init(&q->cond);
    return 0;
}

int queue_is_empty(Queue* q) {
    int is_empty = q->size == 0;
    return is_empty;
}

int queue_is_full(Queue* q) {
    int is_full = q->size == q->capacity;
    return is_full;
}

int queue_enqueue(Queue* q, int item) {
    mtx_lock(&q->lock);
//    Wait for queue to not be full, and insert a new item
    while (queue_is_full(q)) {
        cnd_wait(&q->cond, &q->lock);
    }
    q->arr[q->rear] = item;
    q->size++;
//    Update rear after the enqueueing. Circular queue - So if we add to the end of `arr`, new rear is start of `arr`
    q->rear = (q->rear + 1) % q->capacity;
    cnd_signal(&q->cond);
    mtx_unlock(&q->lock);
    return 0;
}

int queue_dequeue(Queue* q) {
    mtx_lock(&q->lock);
//    Wait for queue to not be empty, and insert a new item
    while (queue_is_empty(q)) {
        cnd_wait(&q->cond, &q->lock);
    }
    int item = q->arr[q->front];
    q->size--;
//    Update front after the dequeueing. Circular queue - So if we remove from the start of `arr`, new front is end of `arr`
    q->front = (q->front + 1) % q->capacity;
    cnd_signal(&q->cond);
    mtx_unlock(&q->lock);
    return item;
}

int queue_free(Queue* q) {
    mtx_destroy(&q->lock);
    cnd_destroy(&q->cond);
    free(q->arr);
    return 0;
}

typedef struct thread_parameters {
    int id;
    Queue* q;
} ThreadParams;

int producer(Queue *arg) {
    Queue* queue = arg;
    for (int i = 0; i < 100; i++) {
        queue_enqueue(queue, i);
        printf("Produced: %d\n", i);
    }
    return 0;
}

int consumer(Queue *arg) {
    Queue* queue = arg;
    int item;
    for (int i = 0; i < 100; i++) {
        item = queue_dequeue(queue);
        printf("Consumed: %d\n", item);
    }
    return 0;
}

void thread_func(const ThreadParams *thread_params)
{
    mtx_lock(&mutex);
    printf("Created thread %d!\n", thread_params->id);
    if(++number_of_ready_threads == number_of_desired_threads) {
        // If we reached here, that means that this is the last thread to reach here, so we signal to main that he should trigger all the waiting threads
        cnd_signal(&is_all_threads_ready);
    }
//    Waiting for a signal from main that we are ready to start.
    cnd_wait(&condition, &mutex);
    mtx_unlock(&mutex);

//    We want half of the threads to be producers for the queue, and half to be consumers of the queue.
    if(thread_params->id % 2)
        producer(thread_params->q);
    else
        consumer(thread_params->q);
}

int main() {
    Queue q;
    int queue_size = 11;
    queue_init(&q, queue_size);

    thrd_t threads[number_of_desired_threads];

    // Initialize mutexes and condition variables for the queue
    mtx_init(&mutex, mtx_plain);
    cnd_init(&condition);
    cnd_init(&is_all_threads_ready);

    // Create `number_of_desired_threads` threads
    for (int i = 0; i < number_of_desired_threads; i++) {
//        Creating an "object" with params we want to  pass to threads
        ThreadParams *thread_params = malloc(sizeof(ThreadParams));
        thread_params->id = i;
        thread_params->q = &q;

        if (thrd_create(&threads[i], (thrd_start_t) thread_func, thread_params) != thrd_success) {
            printf("Error creating thread\n");
            return -1;
        }
    }

    mtx_lock(&mutex);
//    Wait until all threads are waiting for you to start running
    cnd_wait(&is_all_threads_ready, &mutex);
//    Trigger all threads!
    cnd_broadcast(&condition);
    mtx_unlock(&mutex);

    // Wait for all threads to finish
    for (int i = 0; i < number_of_desired_threads; i++) {
        thrd_join(threads[i], NULL);
    }

//    Print experiment result
    printf("All threads has finished!\n");
    if(queue_is_empty(&q))
        printf("Queue is empty!!!\n");
    else
        printf("Queue is not empty!!!\n");

//    Cleanup
    mtx_destroy(&mutex);
    cnd_destroy(&condition);
    cnd_destroy(&is_all_threads_ready);
    queue_free(&q);
    return 0;
}