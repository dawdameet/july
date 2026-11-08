#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <spawn.h>
#include "task.h"

#define PORT 8080
#define BUFFER_SIZE 2048
#define MAX_QUEUE 65536
#define NUM_WORKERS 4

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

extern char **environ;

int execute_task(const Task *task, char *out_buf, size_t out_buf_size)
{
    if (likely(task->taskCommand[0] == ':' && task->taskCommand[1] == '\0'))
    {
        if (out_buf_size > 0)
            out_buf[0] = '\0';
        return 0;
    }

    int pipefd[2];
    if (unlikely(pipe(pipefd) < 0))
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char *argv[] = {"sh", "-c", (char *)task->taskCommand, NULL};
    pid_t pid;
    int err = posix_spawn(&pid, "/bin/sh", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (unlikely(err != 0))
    {
        close(pipefd[0]);
        return -1;
    }

    size_t total = 0;
    while (total < out_buf_size - 1)
    {
        ssize_t n = read(pipefd[0], out_buf + total, out_buf_size - 1 - total);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }
    out_buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

typedef struct
{
    Task tasks[MAX_QUEUE];
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} TaskQueue;

static TaskQueue queue = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER
};

int enqueue_task(const Task *task)
{
    pthread_mutex_lock(&queue.lock);
    if (unlikely(queue.count == MAX_QUEUE))
    {
        pthread_mutex_unlock(&queue.lock);
        return -1;
    }
    queue.tasks[queue.tail] = *task;
    queue.tail = (queue.tail + 1) % MAX_QUEUE;
    queue.count++;
    /* Only signal condition variable when transition from 0 to 1 saves hundreds of thousands of pthread signals */
    if (queue.count == 1)
    {
        pthread_cond_signal(&queue.not_empty);
    }
    pthread_mutex_unlock(&queue.lock);
    return 0;
}

void *worker_thread(void *arg)
{
    (void)arg;
    char output[4096];

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
        pthread_mutex_unlock(&queue.lock);

        output[0] = '\0';
        execute_task(&task, output, sizeof(output));
    }
    return NULL;
}

int parse_task(const char *body, size_t body_len, Task *task)
{
    if (unlikely(body_len < sizeof(TaskHeader)))
        return -1;

    memcpy(&task->header, body, sizeof(TaskHeader));
    uint32_t id_len = task->header.taskIdLen;
    uint32_t command_len = task->header.taskCommandLen;

    if (unlikely(id_len >= MAX_TASK_ID || command_len >= MAX_TASK_CMD))
        return -1;

    size_t payload_len = (size_t)id_len + (size_t)command_len;
    if (unlikely(body_len - sizeof(TaskHeader) < payload_len))
        return -1;

    const char *curr = body + sizeof(TaskHeader);
    memcpy(task->taskId, curr, id_len);
    task->taskId[id_len] = '\0';
    curr += id_len;
    memcpy(task->taskCommand, curr, command_len);
    task->taskCommand[command_len] = '\0';
    return 0;
}

typedef struct Connection
{
    int fd;
    char input[BUFFER_SIZE];
    size_t input_len;
    char output[128];
    size_t output_len;
    size_t output_offset;
    size_t header_bytes;
    size_t content_len;
    bool close_after_write;
} Connection;

static const char response_200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "Hello, World!";
static const size_t response_200_len = sizeof(response_200) - 1;

static const char response_202[] =
    "HTTP/1.1 202 Accepted\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "Task Queued";
static const size_t response_202_len = sizeof(response_202) - 1;

static const char response_400[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const size_t response_400_len = sizeof(response_400) - 1;

static const char response_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const size_t response_404_len = sizeof(response_404) - 1;

static const char response_503[] =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
static const size_t response_503_len = sizeof(response_503) - 1;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (unlikely(flags < 0))
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int update_interest(int kq, Connection *conn, int filter)
{
    struct kevent changes[2];
    int other = (filter == EVFILT_READ) ? EVFILT_WRITE : EVFILT_READ;
    EV_SET(&changes[0], conn->fd, filter, EV_ENABLE, 0, 0, conn);
    EV_SET(&changes[1], conn->fd, other, EV_DISABLE, 0, 0, conn);
    return kevent(kq, changes, 2, NULL, 0, NULL);
}

static inline void close_connection(int kq, Connection *conn)
{
    (void)kq;
    if (!conn)
        return;
    close(conn->fd);
    free(conn);
}

static inline ssize_t complete_request_size(Connection *conn)
{
    conn->input[conn->input_len] = '\0';
    char *header_end = strstr(conn->input, "\r\n\r\n");
    if (!header_end)
        return conn->input_len >= BUFFER_SIZE - 1 ? -1 : 0;

    size_t header_bytes = (size_t)(header_end + 4 - conn->input);
    size_t content_len = 0;
    char *cl = strcasestr(conn->input, "Content-Length:");
    if (cl && cl < header_end)
    {
        char *val = cl + 15;
        while (*val == ' ')
            val++;
        while (*val >= '0' && *val <= '9')
            content_len = content_len * 10 + (size_t)(*val++ - '0');
    }

    size_t total = header_bytes + content_len;
    if (unlikely(total >= BUFFER_SIZE))
        return -1;

    if (conn->input_len >= total)
    {
        conn->header_bytes = header_bytes;
        conn->content_len = content_len;
        return (ssize_t)total;
    }
    return 0;
}

static inline bool request_wants_close(const char *request, size_t header_len)
{
    const char *cl = strcasestr(request, "Connection:");
    if (cl && (size_t)(cl - request) < header_len)
    {
        const char *val = cl + 11;
        while (*val == ' ' || *val == '\t')
            val++;
        if (strncasecmp(val, "close", 5) == 0)
            return true;
    }
    return false;
}

static inline void write_response(Connection *conn, const char *src, size_t len)
{
    conn->output_len = len;
    memcpy(conn->output, src, len);
    conn->output_offset = 0;
}

static int prepare_response(Connection *conn, size_t request_len)
{
    conn->input[request_len] = '\0';
    conn->close_after_write = request_wants_close(conn->input, conn->header_bytes);

    if (conn->input[0] == 'G' && strncmp(conn->input, "GET / ", 6) == 0)
    {
        write_response(conn, response_200, response_200_len);
    }
    else if (conn->input[0] == 'P' && strncmp(conn->input, "POST /task ", 11) == 0)
    {
        char *body = conn->input + conn->header_bytes;
        size_t declared_len = conn->content_len;
        size_t received_body_len = request_len - conn->header_bytes;
        Task task;
        if (likely(received_body_len >= declared_len &&
                   parse_task(body, declared_len, &task) == 0))
        {
            if (likely(enqueue_task(&task) == 0))
                write_response(conn, response_202, response_202_len);
            else
                write_response(conn, response_503, response_503_len);
        }
        else
        {
            write_response(conn, response_400, response_400_len);
        }
    }
    else
    {
        write_response(conn, response_404, response_404_len);
    }

    conn->input_len -= request_len;
    if (conn->input_len > 0)
    {
        memmove(conn->input, conn->input + request_len, conn->input_len);
    }
    conn->input[conn->input_len] = '\0';
    return 0;
}

static inline int flush_response(Connection *conn)
{
    while (conn->output_offset < conn->output_len)
    {
        ssize_t n = send(conn->fd,
                         conn->output + conn->output_offset,
                         conn->output_len - conn->output_offset,
                         0);
        if (likely(n > 0))
        {
            conn->output_offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        return -1;
    }
    return 0;
}

static volatile sig_atomic_t g_running = 1;
static void handle_sig(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(void)
{
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket failed");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = {0};
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);
    address.sin_family = AF_INET;

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (listen(server_fd, 4096) < 0)
    {
        perror("listen failed");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (set_nonblocking(server_fd) < 0)
    {
        perror("nonblocking listener failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++)
    {
        int rc = pthread_create(&workers[i], NULL, worker_thread, NULL);
        if (rc != 0)
        {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            close(server_fd);
            return EXIT_FAILURE;
        }
        pthread_detach(workers[i]);
    }

    int kq = kqueue();
    if (kq < 0)
    {
        perror("kqueue failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct kevent listen_event;
    EV_SET(&listen_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kq, &listen_event, 1, NULL, 0, NULL) < 0)
    {
        perror("kevent listener failed");
        close(kq);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("July optimized kqueue server on http://localhost:%d with %d workers\n",
           PORT, NUM_WORKERS);

    struct kevent events[256];
    while (g_running)
    {
        int ready = kevent(kq, NULL, 0, events, 256, NULL);
        if (unlikely(ready < 0))
        {
            if (errno == EINTR)
                continue;
            perror("kevent wait failed");
            break;
        }

        for (int i = 0; i < ready; i++)
        {
            if (events[i].udata == NULL)
            {
                for (;;)
                {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        break;
                    }
                    if (unlikely(set_nonblocking(client_fd) < 0))
                    {
                        close(client_fd);
                        continue;
                    }

                    int nosigpipe = 1;
                    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE,
                               &nosigpipe, sizeof(nosigpipe));

                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                               &nodelay, sizeof(nodelay));

                    Connection *conn = calloc(1, sizeof(*conn));
                    if (unlikely(!conn))
                    {
                        close(client_fd);
                        continue;
                    }
                    conn->fd = client_fd;

                    struct kevent client_events[2];
                    EV_SET(&client_events[0], client_fd, EVFILT_READ,
                           EV_ADD | EV_ENABLE, 0, 0, conn);
                    EV_SET(&client_events[1], client_fd, EVFILT_WRITE,
                           EV_ADD | EV_DISABLE, 0, 0, conn);
                    if (unlikely(kevent(kq, client_events, 2, NULL, 0, NULL) < 0))
                    {
                        close(client_fd);
                        free(conn);
                    }
                }
                continue;
            }

            Connection *conn = events[i].udata;
            if (unlikely(events[i].flags & (EV_ERROR | EV_EOF)))
            {
                close_connection(kq, conn);
                continue;
            }

            bool failed = false;
            if (events[i].filter == EVFILT_READ)
            {
                for (;;)
                {
                    if (unlikely(conn->input_len >= BUFFER_SIZE - 1))
                    {
                        failed = true;
                        break;
                    }
                    ssize_t n = recv(conn->fd,
                                     conn->input + conn->input_len,
                                     BUFFER_SIZE - 1 - conn->input_len, 0);
                    if (likely(n > 0))
                    {
                        conn->input_len += (size_t)n;
                        conn->input[conn->input_len] = '\0';
                        continue;
                    }
                    if (n == 0)
                    {
                        failed = true;
                        break;
                    }
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    failed = true;
                    break;
                }

                if (!failed && conn->input_len > 0)
                {
                    ssize_t request_len = complete_request_size(conn);
                    if (request_len < 0)
                        failed = true;
                    else if (request_len > 0)
                    {
                        prepare_response(conn, (size_t)request_len);
                        int wr = flush_response(conn);
                        if (wr < 0)
                            failed = true;
                        else if (wr == 1)
                        {
                            if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                                failed = true;
                        }
                        else if (conn->close_after_write)
                        {
                            close_connection(kq, conn);
                            continue;
                        }
                        else
                        {
                            ssize_t next_len = complete_request_size(conn);
                            if (next_len > 0)
                            {
                                prepare_response(conn, (size_t)next_len);
                                if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                                    failed = true;
                            }
                            else if (next_len < 0)
                                failed = true;
                            else if (update_interest(kq, conn, EVFILT_READ) < 0)
                                failed = true;
                        }
                    }
                }
            }

            if (!failed && (events[i].filter == EVFILT_WRITE))
            {
                int wr = flush_response(conn);
                if (wr < 0)
                    failed = true;
                else if (wr == 0)
                {
                    if (conn->close_after_write)
                    {
                        close_connection(kq, conn);
                        continue;
                    }
                    ssize_t next_len = complete_request_size(conn);
                    if (next_len > 0)
                    {
                        prepare_response(conn, (size_t)next_len);
                        if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                            failed = true;
                    }
                    else if (next_len < 0)
                        failed = true;
                    else if (update_interest(kq, conn, EVFILT_READ) < 0)
                        failed = true;
                }
            }

            if (failed)
                close_connection(kq, conn);
        }
    }

    close(kq);
    close(server_fd);
    return 0;
}
