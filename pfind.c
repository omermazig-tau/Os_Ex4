#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <threads.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdatomic.h>
#include <dirent.h>
#include <limits.h>


mtx_t mutex;
cnd_t condition;
cnd_t is_all_threads_ready;
atomic_long number_of_files_found = 0;
atomic_long number_of_waiting_threads = 0;
atomic_long number_of_running_threads = 0;
bool is_any_errors = false;
long number_of_ready_threads = 0;
long number_of_desired_threads;
char *search_term;

typedef char *Path;
typedef char *DirPath;
typedef char *FilePath;
typedef struct queue {
    Path *arr;
    size_t size;
    size_t capacity;
    size_t front;
    size_t rear;
    mtx_t lock;
    cnd_t cond;
} Queue;

int queue_init(Queue *q, size_t capacity) {
    q->arr = malloc(capacity * sizeof(char) * PATH_MAX);
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

int queue_free(Queue *q) {
    mtx_destroy(&q->lock);
    cnd_destroy(&q->cond);
    for (size_t i = 0; i < q->size; i++) {
        free(q->arr[i]);
    }
    free(q->arr);
    return 0;
}

int queue_is_empty(Queue *q) {
    int is_empty = q->size == 0;
    return is_empty;
}

int queue_is_full(Queue *q) {
    int is_full = q->size == q->capacity;
    return is_full;
}

int queue_enqueue(Queue *q, Path item) {
    mtx_lock(&q->lock);
//    Wait for queue to not be full, and insert a new item
    while (queue_is_full(q)) {
        cnd_wait(&q->cond, &q->lock);
    }
    q->arr[q->rear] = strdup(item);
    q->size++;
//    Update rear after the enqueueing. Circular queue - So if we add to the end of `arr`, new rear is start of `arr`
    q->rear = (q->rear + 1) % q->capacity;
    cnd_signal(&q->cond);
    mtx_unlock(&q->lock);
    return 0;
}

Path queue_dequeue(Queue *q) {
    mtx_lock(&q->lock);
//    Wait for queue to not be empty, and insert a new item
    while (queue_is_empty(q)) {
//        If the number of waiting threads is `number_of_desired_threads` - 1, that means that every thread got to
//        number_of_waiting_threads++, But no thread got to number_of_waiting_threads--. This means that all thread except this one are
//        waiting, and because the queue is empty, that means I'm going to be waiting too, for no one. So we are done.
        if (number_of_waiting_threads == number_of_running_threads - 1) {
            number_of_running_threads--;
//            This will cause the one thread to come out of the wait, loop back to this if scope (because the queue is
//            still empty), and enter here too (because both `number_of_waiting_threads` and `number_of_running_threads`
//            were decreased by 1). and so on and so on...
            cnd_signal(&q->cond);
            mtx_unlock(&q->lock);
            thrd_exit(is_any_errors);
        }
        else{
//            We should still run, so we wait for something for the queue to be released
            number_of_waiting_threads++;
            cnd_wait(&q->cond, &q->lock);
            number_of_waiting_threads--;
        }
    }
    Path item = strdup(q->arr[q->front]);
    free(q->arr[q->front]);
    q->size--;
//    Update front after the dequeueing. Circular queue - So if we remove from the start of `arr`, new front is end of `arr`
    q->front = (q->front + 1) % q->capacity;
    cnd_signal(&q->cond);
    mtx_unlock(&q->lock);
    return item;
}

bool is_directory_searchable(DirPath dir_path) {
    // Check if the running process has both read and execute permissions for the directory
    return access(dir_path, R_OK | X_OK) == 0;
}

bool is_directory(struct stat s) {
    // Check if the file at the given path is a directory
    if (S_ISDIR(s.st_mode)) {
        return true;
    } else {
        return false;
    }
}

bool is_file_match(FilePath path) {
    // Extract the basename of the file from the path
    char *base = path;
    char *last_slash = strrchr(path, '/');
    if (last_slash != NULL) {
        base = last_slash + 1;
    }

    // Check if the basename contains the search string
    return strstr(base, search_term) != NULL;
}

void process_directory(Path dir_path, Queue *q) {
    // Open the directory specified by the path
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Directory %s: opendir failed.\n", dir_path);
        is_any_errors = true;
        return;
    }

    // Read the entries in the directory one by one
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char full_path[PATH_MAX];
            sprintf(full_path, "%s/%s", dir_path, entry->d_name);
            queue_enqueue(q, full_path);
        }
    }
    closedir(dir);
}

int handle_single_path_item(Queue *arg) {
    Queue *queue = arg;
    Path item;
//    TODO - This is bad. try to avoid while true.
    while (true) {
        item = queue_dequeue(queue);
        struct stat s;
        if (stat(item, &s) != 0) {
            fprintf(stderr, "Error: stat action on path %s failed.\n", item);
            is_any_errors = true;
        }
        else if (is_directory(s)) {
            if (is_directory_searchable(item)) {
                process_directory(item, queue);
            } else {
                fprintf(stderr, "Directory %s: Permission denied.\n", item);
                is_any_errors = true;
            }
        } else {
//            It's a file
            if (is_file_match(item)) {
                number_of_files_found++;
                printf("%s\n", item);
            }
        }
        free(item);
    }
}

void thread_func(Queue *q) {
    mtx_lock(&mutex);
    if (++number_of_ready_threads == number_of_desired_threads) {
        // If we reached here, that means that this is the last thread to reach here, so we signal to main that he should trigger all the waiting threads
        cnd_signal(&is_all_threads_ready);
    }
//    Waiting for a signal from main that we are ready to start.
    cnd_wait(&condition, &mutex);
    mtx_unlock(&mutex);

//    We want half of the threads to be producers for the queue, and half to be consumers of the queue.
    handle_single_path_item(q);
}

int main(int argc, char *argv[]) {
    // Check that the correct number of arguments are passed
    if (argc != 4) {
        fprintf(stderr, "Usage: %s root_directory search_term num_threads\n", argv[0]);
        return 1;
    }

    // Get the root directory, search term, and number of threads from the command line arguments
    Path root_directory = argv[1];
    search_term = argv[2];
    number_of_desired_threads = atoi(argv[3]); // NOLINT(cert-err34-c)

    struct stat s;
    if (stat(root_directory, &s) != 0) {
        fprintf(stderr, "Error: stat action on path %s failed.\n", root_directory);
        return 1;
    }
    if (is_directory(s) && !is_directory_searchable(root_directory)) {
        fprintf(stderr, "Unsearchable root directory %s\n", root_directory);
        return 1;
    }

    Queue q;
    int queue_size = 100000;
    if(queue_init(&q, queue_size) != 0){
        fprintf(stderr, "Couldn't allocate memory for a queue of size %d\n", queue_size);
        return 1;
    }

    thrd_t threads[number_of_desired_threads];

    // Initialize mutexes and condition variables for the queue
    mtx_init(&mutex, mtx_plain);
    cnd_init(&condition);
    cnd_init(&is_all_threads_ready);

    // Create `number_of_desired_threads` threads
    for (int i = 0; i < number_of_desired_threads; i++) {
        if (thrd_create(&threads[i], (thrd_start_t) thread_func, &q) != thrd_success) {
            fprintf(stderr, "Error creating thread %d\n", i);
            return 1;
        }
        else{
            number_of_running_threads++;
        }
    }

    mtx_lock(&mutex);
//    Wait until all threads are waiting for you to start running
    cnd_wait(&is_all_threads_ready, &mutex);
//    Trigger all threads!
    cnd_broadcast(&condition);
    mtx_unlock(&mutex);
    queue_enqueue(&q, root_directory);

    // Wait for all threads to finish
    for (int i = 0; i < number_of_desired_threads; i++) {
        thrd_join(threads[i], NULL);
    }
//    Print conclusion
    printf("Done searching, found %lu files\n", number_of_files_found);

//    Cleanup
    mtx_destroy(&mutex);
    cnd_destroy(&condition);
    cnd_destroy(&is_all_threads_ready);
    queue_free(&q);
    return is_any_errors;
}