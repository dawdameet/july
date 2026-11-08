#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include "task.h"

#define PORT 8080
#define BUFFER_SIZE 1024

#include <pthread.h>
#include <stdbool.h>

#define MAX_QUEUE 64
#define NUM_WORKERS 4

// Add these two prototypes above worker_thread()
int execute_task(const Task *task, char *out_buf, size_t out_buf_size);
void free_task(Task *task);

typedef struct
{
    Task tasks[MAX_QUEUE];
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} TaskQueue;

TaskQueue queue = {.lock = PTHREAD_MUTEX_INITIALIZER,
                   .not_empty = PTHREAD_COND_INITIALIZER,
                   .not_full = PTHREAD_COND_INITIALIZER};

// Called by HTTP main thread (returns 0 on success, -1 if queue is full)
int enqueue_task(Task task)
{
    pthread_mutex_lock(&queue.lock);
    if (queue.count == MAX_QUEUE)
    {
        pthread_mutex_unlock(&queue.lock);
        return -1; // Server busy (return 503 Service Unavailable)
    }
    queue.tasks[queue.tail] = task; // Transfers ownership of malloc'd pointers
    queue.tail = (queue.tail + 1) % MAX_QUEUE;
    queue.count++;
    pthread_cond_signal(&queue.not_empty);
    pthread_mutex_unlock(&queue.lock);
    return 0;
}

// Background worker thread loop
void *worker_thread(void *arg)
{
    (void)arg;
    while (1)
    {
        pthread_mutex_lock(&queue.lock);
        while (queue.count == 0)
        {
            pthread_cond_wait(&queue.not_empty, &queue.lock);
        }
        Task task = queue.tasks[queue.head];
        queue.head = (queue.head + 1) % MAX_QUEUE;
        queue.count--;
        pthread_cond_signal(&queue.not_full);
        pthread_mutex_unlock(&queue.lock);

        // Execute outside the lock so multiple commands run in parallel
        char output[4096] = {0};
        int status = execute_task(&task, output, sizeof(output));
        printf("[Worker %lu] Task %s finished (exit=%d)\n",
               (unsigned long)pthread_self(), task.taskId, status);

        // TODO: Save output/status to a thread-safe result table
        free_task(&task);
    }
    return NULL;
}

int execute_task(const Task *task, char *out_buf, size_t out_buf_size)
{
    printf("[Worker] Executing Task %s: %s\n", task->taskId, task->taskCommand);

    // "r" opens a pipe to read the command's standard output
    FILE *pipe = popen(task->taskCommand, "r");
    if (!pipe)
    {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < out_buf_size - 1)
    {
        size_t n = fread(out_buf + total_read, 1, out_buf_size - 1 - total_read, pipe);
        if (n == 0)
        {
            break;
        }
        total_read += n;
    }
    out_buf[total_read] = '\0';

    int status = pclose(pipe);
    return status; // 0 typically means command succeeded
}

int extract_body(char *buffer, char **body, size_t *body_len)
{
    // we take the raw buffer
    // we are assuming VALID request
    /*
      "POST /task HTTP/1.1\r\n"
      "Host: localhost:8080\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Length: 24\r\n"
      "\r\n"
      "<binary task payload>"
     */
    // first 4 bytes are POST (guarenteed)
    /* while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered first space
     buffer++;// consume space
     while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered second space
     while (strcmp(buffer, ' ')!= 0){buffer++}; //we encountered third space (after host)
     fuck it */

    char *header_end = strstr(buffer, "\r\n\r\n");
    // got there bitchh
    if (!header_end)
    {
        return -1;
    }
    *body = header_end + 4; // consume \r\n\r\n
    char *cl_header = strstr(buffer, "Content-Length");
    if (cl_header && cl_header < header_end)
    {
        *body_len = (size_t)strtoull(cl_header + 15, NULL, 10);
    }
    else
    {
        *body_len = 0;
    }
    return 0;
}
int parse_task(const char *body, size_t body_len, Task *task)
{
    if (body_len < sizeof(TaskHeader))
    {
        return -1;
    }
    memcpy(&task->header, body, sizeof(TaskHeader));
    uint32_t id_len = task->header.taskIdLen;
    uint32_t command_len = task->header.taskCommandLen;
    size_t payload_len = (size_t)id_len + (size_t)command_len;
    if (body_len - sizeof(TaskHeader) < payload_len)
    {
        return -1;
    }
    const char *curr = body + sizeof(TaskHeader);
    task->taskId = malloc(id_len + 1);
    task->taskCommand = malloc(command_len + 1);
    if (!task->taskId || !task->taskCommand)
    {
        free(task->taskId);
        free(task->taskCommand);
        return -1;
    }
    memcpy(task->taskId, curr, id_len);
    task->taskId[id_len] = '\0';
    curr += id_len;
    memcpy(task->taskCommand, curr, command_len);
    task->taskCommand[command_len] = '\0';
    return 0;
}

void free_task(Task *task)
{
    free(task->taskId);
    free(task->taskCommand);
    task->taskId = NULL;
    task->taskCommand = NULL;
}

int main(void)
{
    int server_fd, client_fd;
    struct sockaddr_in address;
    socklen_t addr_len = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    const char *response_200 =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";

    const char *response_202 =
        "HTTP/1.1 202 Accepted\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Task Queued";

    const char *response_400 =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    const char *response_404 =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    const char *response_503 =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    // 1. Spawn the worker thread pool
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0)
        {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
        pthread_detach(workers[i]);
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    address.sin_family = AF_INET;

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 16) < 0)
    {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server running on http://localhost:%d with %d workers\n", PORT, NUM_WORKERS);
    while (1)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&address, &addr_len);
        if (client_fd < 0)
        {
            perror("accept failed");
            continue;
        }

        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read <= 0)
        {
            close(client_fd);
            continue;
        }

        if (strncmp(buffer, "GET / ", 6) == 0)
        {
            write(client_fd, response_200, strlen(response_200));
        }
        else if (strncmp(buffer, "POST /task ", 11) == 0)
        {
            char *body = NULL;
            size_t declared_len = 0;

            if (extract_body(buffer, &body, &declared_len) == 0)
            {
                size_t received_body_len = (size_t)((buffer + bytes_read) - body);
                Task task = {0};

                if (received_body_len >= declared_len &&
                    parse_task(body, declared_len, &task) == 0)
                {
                    if (enqueue_task(task) == 0)
                    {
                        // Worker thread now owns `task` and will call free_task()
                        write(client_fd, response_202, strlen(response_202));
                    }
                    else
                    {
                        // Queue is full; main thread still owns `task`
                        free_task(&task);
                        write(client_fd, response_503, strlen(response_503));
                    }
                }
                else
                {
                    write(client_fd, response_400, strlen(response_400));
                }
            }
            else
            {
                write(client_fd, response_400, strlen(response_400));
            }
        }
        else
        {
            write(client_fd, response_404, strlen(response_404));
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
