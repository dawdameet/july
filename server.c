#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <spawn.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <poll.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#else
#include <sched.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#endif

#include "task.h"
#include "workflow.h"

static char g_auth_token[256] = {0};

static inline bool is_request_authenticated(const char *headers, size_t header_len)
{
    if (g_auth_token[0] == '\0')
        return true; // Open LAN mode

    if (!headers || header_len == 0)
        return false;

    // 1. Check "X-July-Token: <token>"
    const char *tok = strcasestr(headers, "X-July-Token:");
    if (tok && (size_t)(tok - headers) < header_len)
    {
        tok += 13;
        while (*tok == ' ' || *tok == '\t') tok++;
        const char *eol = strpbrk(tok, "\r\n");
        size_t tok_len = eol ? (size_t)(eol - tok) : strlen(tok);
        if (tok_len == strlen(g_auth_token) && strncmp(tok, g_auth_token, tok_len) == 0)
            return true;
    }

    // 2. Check "Authorization: Bearer <token>"
    const char *auth = strcasestr(headers, "Authorization: Bearer ");
    if (auth && (size_t)(auth - headers) < header_len)
    {
        auth += 22;
        while (*auth == ' ' || *auth == '\t') auth++;
        const char *eol = strpbrk(auth, "\r\n");
        size_t tok_len = eol ? (size_t)(eol - auth) : strlen(auth);
        if (tok_len == strlen(g_auth_token) && strncmp(auth, g_auth_token, tok_len) == 0)
            return true;
    }

    return false;
}

static const char response_401[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 32\r\n"
    "Connection: close\r\n"
    "\r\n"
    "401 Unauthorized: Invalid Token\n";
static const size_t response_401_len = sizeof(response_401) - 1;

#define PORT 8080
#define INITIAL_BUFFER_SIZE 8192
#define MAX_PAYLOAD_SIZE (64 * 1024 * 1024) // 64 MB max envelope
#define OUTPUT_BUFFER_SIZE 8192
#define MAX_QUEUE 65536
#define NUM_WORKERS 4
#define WORKER_BATCH 16
#define MAX_TASK_OUTPUT 4096

#define NUM_STRIPES 64
#define ENTRIES_PER_STRIPE 128

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

extern char **environ;

/* ---------------- Task Result Table (Striped for Concurrency) ---------------- */

typedef enum {
    TASK_STATUS_QUEUED = 1,
    TASK_STATUS_RUNNING,
    TASK_STATUS_COMPLETED
} TaskStatus;

#define MAX_RECENT_TASKS 64

typedef struct TaskResult {
    char taskId[MAX_TASK_ID];
    char label[64];
    bool is_workflow;
    bool killed;
    TaskStatus status;
    int exit_code;
    int artifacts_count;
    pid_t active_pgid;
    time_t created_at;
    time_t finished_at;
    size_t output_len;
    char output[MAX_TASK_OUTPUT];
    int next; // index within this stripe's entries
} TaskResult;

static time_t g_server_start_time = 0;
static pthread_mutex_t g_recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_recent_tasks[MAX_RECENT_TASKS][MAX_TASK_ID];
static int g_recent_head = 0;
static int g_recent_count = 0;

static void record_recent_task_id(const char *task_id)
{
    pthread_mutex_lock(&g_recent_lock);
    for (int i = 0; i < g_recent_count; i++)
    {
        if (strncmp(g_recent_tasks[i], task_id, MAX_TASK_ID) == 0)
        {
            pthread_mutex_unlock(&g_recent_lock);
            return;
        }
    }
    snprintf(g_recent_tasks[g_recent_head], MAX_TASK_ID, "%s", task_id);
    g_recent_head = (g_recent_head + 1) % MAX_RECENT_TASKS;
    if (g_recent_count < MAX_RECENT_TASKS) g_recent_count++;
    pthread_mutex_unlock(&g_recent_lock);
}

typedef struct {
    pthread_mutex_t lock;
    int cursor;
    int buckets[ENTRIES_PER_STRIPE];
    TaskResult entries[ENTRIES_PER_STRIPE];
} ResultStripe;

static ResultStripe g_stripes[NUM_STRIPES];

static inline unsigned int hash_task_id(const char *str)
{
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static void init_result_store(void)
{
    g_server_start_time = time(NULL);
    for (int s = 0; s < NUM_STRIPES; s++)
    {
        pthread_mutex_init(&g_stripes[s].lock, NULL);
        g_stripes[s].cursor = 0;
        for (int i = 0; i < ENTRIES_PER_STRIPE; i++)
        {
            g_stripes[s].buckets[i] = -1;
            g_stripes[s].entries[i].next = -1;
            g_stripes[s].entries[i].status = 0;
        }
    }
}

static void task_result_record_enqueued(const char *task_id)
{
    record_recent_task_id(task_id);
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            st->entries[idx].status = TASK_STATUS_QUEUED;
            st->entries[idx].exit_code = 0;
            st->entries[idx].artifacts_count = 0;
            st->entries[idx].active_pgid = 0;
            st->entries[idx].killed = false;
            st->entries[idx].created_at = time(NULL);
            st->entries[idx].finished_at = 0;
            st->entries[idx].output_len = 0;
            st->entries[idx].output[0] = '\0';
            pthread_mutex_unlock(&st->lock);
            return;
        }
        idx = st->entries[idx].next;
    }

    idx = st->cursor;
    st->cursor = (st->cursor + 1) % ENTRIES_PER_STRIPE;

    if (st->entries[idx].status != 0)
    {
        unsigned int old_h = hash_task_id(st->entries[idx].taskId);
        unsigned int old_b = (old_h >> 6) % ENTRIES_PER_STRIPE;
        int *curr = &st->buckets[old_b];
        while (*curr != -1)
        {
            if (*curr == idx)
            {
                *curr = st->entries[idx].next;
                break;
            }
            curr = &st->entries[*curr].next;
        }
    }

    size_t id_len = strlen(task_id);
    if (id_len >= MAX_TASK_ID)
        id_len = MAX_TASK_ID - 1;
    memcpy(st->entries[idx].taskId, task_id, id_len);
    st->entries[idx].taskId[id_len] = '\0';
    st->entries[idx].label[0] = '\0';
    st->entries[idx].is_workflow = (strncmp(task_id, "wf_", 3) == 0);
    st->entries[idx].killed = false;
    st->entries[idx].status = TASK_STATUS_QUEUED;
    st->entries[idx].exit_code = 0;
    st->entries[idx].artifacts_count = 0;
    st->entries[idx].active_pgid = 0;
    st->entries[idx].created_at = time(NULL);
    st->entries[idx].finished_at = 0;
    st->entries[idx].output_len = 0;
    st->entries[idx].output[0] = '\0';
    st->entries[idx].next = st->buckets[b];
    st->buckets[b] = idx;

    pthread_mutex_unlock(&st->lock);
}

static void task_result_set_meta(const char *task_id, const char *label, bool is_workflow)
{
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            if (label && label[0] != '\0' && st->entries[idx].label[0] == '\0')
                snprintf(st->entries[idx].label, sizeof(st->entries[idx].label), "%s", label);
            st->entries[idx].is_workflow = is_workflow;
            break;
        }
        idx = st->entries[idx].next;
    }
    pthread_mutex_unlock(&st->lock);
}

static void task_result_set_pgid(const char *task_id, pid_t pgid)
{
    if (!task_id || task_id[0] == '\0') return;
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            st->entries[idx].active_pgid = pgid;
            break;
        }
        idx = st->entries[idx].next;
    }
    pthread_mutex_unlock(&st->lock);
}

static bool task_result_kill(const char *task_id)
{
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;
    pid_t pgid = 0;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            pgid = st->entries[idx].active_pgid;
            if (pgid > 0) st->entries[idx].killed = true;
            break;
        }
        idx = st->entries[idx].next;
    }
    pthread_mutex_unlock(&st->lock);

    if (pgid > 0)
    {
        kill(-pgid, SIGKILL);
        return true;
    }
    return false;
}

static void task_result_record_running(const char *task_id)
{
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            st->entries[idx].status = TASK_STATUS_RUNNING;
            break;
        }
        idx = st->entries[idx].next;
    }
    pthread_mutex_unlock(&st->lock);
}

static void task_result_record_completed_full(const char *task_id, int exit_code, int artifacts_count,
                                             const char *output, size_t out_len)
{
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            st->entries[idx].status = TASK_STATUS_COMPLETED;
            st->entries[idx].exit_code = exit_code;
            st->entries[idx].artifacts_count = artifacts_count;
            st->entries[idx].active_pgid = 0;
            st->entries[idx].finished_at = time(NULL);
            if (out_len >= MAX_TASK_OUTPUT)
                out_len = MAX_TASK_OUTPUT - 1;
            memcpy(st->entries[idx].output, output, out_len);
            st->entries[idx].output[out_len] = '\0';
            st->entries[idx].output_len = out_len;
            break;
        }
        idx = st->entries[idx].next;
    }

    if (idx == -1)
    {
        idx = st->cursor;
        st->cursor = (st->cursor + 1) % ENTRIES_PER_STRIPE;

        if (st->entries[idx].status != 0)
        {
            unsigned int old_h = hash_task_id(st->entries[idx].taskId);
            unsigned int old_b = (old_h >> 6) % ENTRIES_PER_STRIPE;
            int *curr = &st->buckets[old_b];
            while (*curr != -1)
            {
                if (*curr == idx)
                {
                    *curr = st->entries[idx].next;
                    break;
                }
                curr = &st->entries[*curr].next;
            }
        }

        size_t id_len = strlen(task_id);
        if (id_len >= MAX_TASK_ID)
            id_len = MAX_TASK_ID - 1;
        memcpy(st->entries[idx].taskId, task_id, id_len);
        st->entries[idx].taskId[id_len] = '\0';
        st->entries[idx].status = TASK_STATUS_COMPLETED;
        st->entries[idx].exit_code = exit_code;
        st->entries[idx].artifacts_count = artifacts_count;
        st->entries[idx].active_pgid = 0;
        st->entries[idx].killed = false;
        st->entries[idx].label[0] = '\0';
        st->entries[idx].is_workflow = (strncmp(task_id, "wf_", 3) == 0);
        st->entries[idx].created_at = time(NULL);
        st->entries[idx].finished_at = st->entries[idx].created_at;
        if (out_len >= MAX_TASK_OUTPUT)
            out_len = MAX_TASK_OUTPUT - 1;
        memcpy(st->entries[idx].output, output, out_len);
        st->entries[idx].output[out_len] = '\0';
        st->entries[idx].output_len = out_len;
        st->entries[idx].next = st->buckets[b];
        st->buckets[b] = idx;
    }

    pthread_mutex_unlock(&st->lock);
}

static inline void task_result_record_completed(const char *task_id, int exit_code,
                                                const char *output, size_t out_len)
{
    task_result_record_completed_full(task_id, exit_code, 0, output, out_len);
}

static int task_result_get(const char *task_id, TaskStatus *status, int *exit_code, int *artifacts_count,
                           char *out_buf, size_t *out_len, size_t max_out)
{
    unsigned int h = hash_task_id(task_id);
    ResultStripe *st = &g_stripes[h % NUM_STRIPES];
    unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

    pthread_mutex_lock(&st->lock);
    int idx = st->buckets[b];
    while (idx != -1)
    {
        if (strncmp(st->entries[idx].taskId, task_id, MAX_TASK_ID) == 0)
        {
            *status = st->entries[idx].status;
            *exit_code = st->entries[idx].exit_code;
            if (artifacts_count)
                *artifacts_count = st->entries[idx].artifacts_count;
            size_t len = st->entries[idx].output_len;
            if (len >= max_out)
                len = max_out - 1;
            memcpy(out_buf, st->entries[idx].output, len);
            out_buf[len] = '\0';
            *out_len = len;
            pthread_mutex_unlock(&st->lock);
            return 0;
        }
        idx = st->entries[idx].next;
    }
    pthread_mutex_unlock(&st->lock);
    return -1;
}

/* ---------------- Process Group & Command Execution in Sandbox ---------------- */

static int run_command_in_sandbox(const char *command, const char *sandbox_dir,
                                  int log_fd, int timeout_sec, bool kill_if_timeout,
                                  const WorkflowManifest *manifest, const char *task_id)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) // Child
    {
        // 1. Process Group Isolation (so kill(-pgid) hits entire tree)
        setpgid(0, 0);

        // 2. Redirect stdin (/dev/null), stdout & stderr
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        // 3. Set Working Directory to Sandbox
        if (chdir(sandbox_dir) != 0)
        {
            perror("chdir to sandbox");
            exit(127);
        }

        // 4. Inject Custom Environment Variables
        if (manifest)
        {
            for (int e = 0; e < manifest->num_env_vars; e++)
            {
                setenv(manifest->env_vars[e].key, manifest->env_vars[e].val, 1);
            }
        }

        // 5. Apply Sandbox Resource Limits (setrlimit)
        if (manifest)
        {
            if (manifest->sandbox_memory_mb > 0)
            {
                struct rlimit rl_mem;
                rl_mem.rlim_cur = (rlim_t)manifest->sandbox_memory_mb * 1024ULL * 1024ULL;
                rl_mem.rlim_max = rl_mem.rlim_cur;
                setrlimit(RLIMIT_DATA, &rl_mem);
            }

            if (manifest->sandbox_max_pids > 0)
            {
                struct rlimit rl_proc;
                rl_proc.rlim_cur = (rlim_t)manifest->sandbox_max_pids;
                rl_proc.rlim_max = (rlim_t)manifest->sandbox_max_pids;
                setrlimit(RLIMIT_NPROC, &rl_proc);
            }

            if (timeout_sec > 0)
            {
                struct rlimit rl_cpu;
                rl_cpu.rlim_cur = (rlim_t)(timeout_sec + 5);
                rl_cpu.rlim_max = (rlim_t)(timeout_sec + 5);
                setrlimit(RLIMIT_CPU, &rl_cpu);
            }
        }

        // 6. Linux Kernel Namespaces Isolation (User, Mount, PID, Network)
#if defined(__linux__)
        if (manifest && manifest->sandbox_namespace)
        {
            int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC;
            if (!manifest->sandbox_network)
            {
                flags |= CLONE_NEWNET; // loopback only
            }
            unshare(CLONE_NEWUSER | flags);
        }
        else if (manifest && !manifest->sandbox_network)
        {
            // Drop network access by default if offline sandbox
            unshare(CLONE_NEWNET);
        }
#endif

        // 7. Execute
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("execl /bin/sh");
        exit(127);
    }

    // Parent
    close(pipefd[1]);
    setpgid(pid, pid);
    task_result_set_pgid(task_id, pid);

    int fl = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int status = 0;
    bool timed_out = false;
    char read_buf[4096];

    while (1)
    {
        while (1)
        {
            ssize_t n = read(pipefd[0], read_buf, sizeof(read_buf));
            if (n > 0)
            {
                write(log_fd, read_buf, (size_t)n);
            }
            else
            {
                break;
            }
        }

        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid)
        {
            break;
        }
        else if (res < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (timeout_sec > 0 && elapsed >= timeout_sec)
        {
            timed_out = true;
            if (kill_if_timeout)
            {
                // Kill entire process tree
                kill(-pid, SIGKILL);
            }
            else
            {
                kill(pid, SIGTERM);
            }
            waitpid(pid, &status, 0);
            dprintf(log_fd, "\n[Timeout] Command exceeded timeout of %d seconds. Process group killed.\n", timeout_sec);
            break;
        }

        // Sub-millisecond reactive wakeup via poll() instead of 10ms idle sleep
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
        int poll_timeout = 5; // max 5ms fallback check
        if (timeout_sec > 0)
        {
            int rem_ms = (int)((timeout_sec - elapsed) * 1000.0);
            if (rem_ms <= 0) poll_timeout = 0;
            else if (rem_ms < poll_timeout) poll_timeout = rem_ms;
        }
        poll(&pfd, 1, poll_timeout);
    }

    // Final drain of any remaining pipe output
    while (1)
    {
        ssize_t n = read(pipefd[0], read_buf, sizeof(read_buf));
        if (n > 0)
        {
            write(log_fd, read_buf, (size_t)n);
        }
        else
        {
            break;
        }
    }
    close(pipefd[0]);
    task_result_set_pgid(task_id, 0);

    if (timed_out)
        return 124;

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0)
    {
        close(in);
        return -1;
    }

    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
    {
        if (write(out, buf, (size_t)n) != n)
        {
            close(in);
            close(out);
            return -1;
        }
    }
    close(in);
    close(out);
    return 0;
}

/* Zero-copy kernel sendfile streaming (from arbitrary byte offset) */
static void send_file_slice_zero_copy(int sock_fd, int file_fd, off_t offset, size_t len)
{
    if (len == 0) return;
#if defined(__APPLE__)
    off_t l = (off_t)len;
    sendfile(file_fd, sock_fd, offset, &l, NULL, 0);
#else
    size_t remaining = len;
    while (remaining > 0)
    {
        ssize_t sent = sendfile(sock_fd, file_fd, &offset, remaining);
        if (sent <= 0)
        {
            if (sent < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            break;
        }
        remaining -= (size_t)sent;
    }
#endif
}

/* Zero-copy kernel sendfile streaming */
static void send_file_zero_copy(int sock_fd, int file_fd, size_t file_size)
{
    send_file_slice_zero_copy(sock_fd, file_fd, 0, file_size);
}

/* ---------------- Workflow Pipeline Execution ---------------- */

int execute_workflow_task(const Task *task)
{
    char storage_dir[512];
    snprintf(storage_dir, sizeof(storage_dir), "/tmp/july_storage/%s", task->taskId);

    char sandbox_dir[512];
    snprintf(sandbox_dir, sizeof(sandbox_dir), "/tmp/july_runs/%s", task->taskId);

    char artifacts_dir[512];
    snprintf(artifacts_dir, sizeof(artifacts_dir), "/tmp/july_storage/%s/artifacts", task->taskId);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "/tmp/july_storage/%s/stdout.log", task->taskId);

    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "/tmp/july_storage/%s/meta.json", task->taskId);

    char envelope_path[512];
    snprintf(envelope_path, sizeof(envelope_path), "/tmp/july_storage/%s/envelope.bin", task->taskId);

    mkdir_p(storage_dir);
    mkdir_p(sandbox_dir);
    mkdir_p(artifacts_dir);

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
        return -1;

    // Read envelope from WAL
    int ef = open(envelope_path, O_RDONLY);
    if (ef < 0)
    {
        dprintf(log_fd, "[Error] Could not open envelope.bin\n");
        close(log_fd);
        task_result_record_completed_full(task->taskId, 1, 0, "Envelope missing\n", 17);
        return -1;
    }

    struct stat est;
    if (fstat(ef, &est) < 0 || est.st_size < 16)
    {
        dprintf(log_fd, "[Error] Invalid envelope.bin size\n");
        close(ef);
        close(log_fd);
        task_result_record_completed_full(task->taskId, 1, 0, "Invalid envelope\n", 17);
        return -1;
    }

    uint8_t *env_data = malloc((size_t)est.st_size);
    if (!env_data)
    {
        close(ef);
        close(log_fd);
        return -1;
    }
    ssize_t r = read(ef, env_data, (size_t)est.st_size);
    close(ef);
    if (r != est.st_size)
    {
        free(env_data);
        close(log_fd);
        return -1;
    }

    // Unpack directly into sandbox without tar subprocesses
    WorkflowManifest manifest;
    if (unpack_envelope(env_data, (size_t)est.st_size, sandbox_dir, &manifest) != 0)
    {
        dprintf(log_fd, "[Error] Failed to unpack binary envelope or parse JSON manifest\n");
        free(env_data);
        close(log_fd);
        task_result_record_completed_full(task->taskId, 1, 0, "Unpack error\n", 13);
        return -1;
    }
    free(env_data);

    if (manifest.is_workspace)
    {
        safe_copy_str(sandbox_dir, sizeof(sandbox_dir), manifest.workspace_dir);
        dprintf(log_fd, "[Workspace] Persistent workspace active: %s\n", sandbox_dir);
    }

    dprintf(log_fd, "=== July Workflow: %s (Task ID: %s) ===\n", manifest.name, task->taskId);
    task_result_set_meta(task->taskId, manifest.name[0] ? manifest.name : task->taskId, true);
    if (manifest.sandbox_memory_mb > 0 || manifest.sandbox_max_pids > 0 || manifest.sandbox_namespace || !manifest.sandbox_network)
    {
        dprintf(log_fd, "[Sandbox] Config: net=%s, mem=%dMB, pids=%s, ns=%s\n",
                manifest.sandbox_network ? "host" : "loopback",
                manifest.sandbox_memory_mb,
                manifest.sandbox_max_pids > 0 ? "limited" : "host-managed",
                manifest.sandbox_namespace ? "enabled" : "disabled");
    }
    if (manifest.num_env_vars > 0)
    {
        dprintf(log_fd, "[Env] Injected %d custom environment variable(s)\n", manifest.num_env_vars);
    }

    int exit_code = 0;

    // Phase 1: Pre-flight tool check
    for (int i = 0; i < manifest.num_check_tools; i++)
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", manifest.check_tools[i]);
        int status = system(cmd);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            dprintf(log_fd, "[Pre-flight] Tool '%s': OK\n", manifest.check_tools[i]);
        }
        else
        {
            dprintf(log_fd, "[Pre-flight] Tool check failed: '%s' not found on host.\n", manifest.check_tools[i]);
            exit_code = 127;
            goto finish_workflow;
        }
    }

    // Phase 2: Dependencies
    if (manifest.from_github && manifest.github_link[0] != '\0')
    {
        dprintf(log_fd, "[Deps] Cloning from GitHub: %s\n", manifest.github_link);
        char git_cmd[1024];
        snprintf(git_cmd, sizeof(git_cmd), "git clone --depth 1 %s .", manifest.github_link);
        int rc = run_command_in_sandbox(git_cmd, sandbox_dir, log_fd, manifest.timeout_sec, manifest.kill_if_timeout, &manifest, task->taskId);
        if (rc != 0)
        {
            dprintf(log_fd, "[Deps] Git clone failed with exit code %d\n", rc);
            exit_code = rc;
            goto finish_workflow;
        }
    }

    for (int i = 0; i < manifest.num_deps; i++)
    {
        dprintf(log_fd, "[Deps] Executing: %s\n", manifest.deps[i]);
        int rc = run_command_in_sandbox(manifest.deps[i], sandbox_dir, log_fd, manifest.timeout_sec, manifest.kill_if_timeout, &manifest, task->taskId);
        if (rc != 0)
        {
            dprintf(log_fd, "[Deps] Dependency failed with exit code %d\n", rc);
            exit_code = rc;
            goto finish_workflow;
        }
    }

    // Phase 3: Build & Execution pipeline
    for (int i = 0; i < manifest.num_build_cmds; i++)
    {
        dprintf(log_fd, "[Build] Executing: %s\n", manifest.build_cmds[i]);
        int rc = run_command_in_sandbox(manifest.build_cmds[i], sandbox_dir, log_fd, manifest.timeout_sec, manifest.kill_if_timeout, &manifest, task->taskId);
        if (rc != 0)
        {
            dprintf(log_fd, "[Build] Command failed with exit code %d\n", rc);
            exit_code = rc;
            goto finish_workflow;
        }
    }

finish_workflow:
    ;
    // Phase 4: Artifact staging
    int staged_count = 0;
    if (exit_code == 0)
    {
        for (int i = 0; i < manifest.num_required_artifacts; i++)
        {
            const char *art = manifest.required_artifacts[i];
            char src_path[1024], dst_path[1024];
            snprintf(src_path, sizeof(src_path), "%s/%s", sandbox_dir, art);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", artifacts_dir, art);

            mkdir_p_parent(dst_path);
            int staged = manifest.is_workspace ? copy_file(src_path, dst_path) :
                         (rename(src_path, dst_path) == 0 ? 0 : copy_file(src_path, dst_path));
            if (staged == 0)
            {
                struct stat ast;
                stat(dst_path, &ast);
                dprintf(log_fd, "[Artifacts] Staged '%s' (%ld bytes)\n", art, (long)ast.st_size);
                staged_count++;
            }
            else
            {
                dprintf(log_fd, "[Artifacts] Warning: Required artifact '%s' could not be found or staged.\n", art);
            }
        }
    }
    else if (manifest.num_failure_artifacts > 0)
    {
        for (int i = 0; i < manifest.num_failure_artifacts; i++)
        {
            const char *art = manifest.failure_artifacts[i];
            char src_path[1024], dst_path[1024];
            snprintf(src_path, sizeof(src_path), "%s/%s", sandbox_dir, art);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", artifacts_dir, art);

            mkdir_p_parent(dst_path);
            int staged = manifest.is_workspace ? copy_file(src_path, dst_path) :
                         (rename(src_path, dst_path) == 0 ? 0 : copy_file(src_path, dst_path));
            if (staged == 0)
            {
                struct stat ast;
                stat(dst_path, &ast);
                dprintf(log_fd, "[Artifacts:Failure] Staged '%s' (%ld bytes)\n", art, (long)ast.st_size);
                staged_count++;
            }
        }
    }

    dprintf(log_fd, "\n=== Workflow Finished (Exit Code: %d, Staged Artifacts: %d) ===\n", exit_code, staged_count);
    close(log_fd);

    // Phase 5: Write metadata (meta.json)
    FILE *mf = fopen(meta_path, "w");
    if (mf)
    {
        fprintf(mf, "{\n");
        fprintf(mf, "  \"taskId\": \"%s\",\n", task->taskId);
        fprintf(mf, "  \"name\": \"%s\",\n", manifest.name);
        fprintf(mf, "  \"status\": \"%s\",\n", exit_code == 0 ? "Completed" : "Failed");
        fprintf(mf, "  \"exit_code\": %d,\n", exit_code);
        fprintf(mf, "  \"artifacts_count\": %d,\n", staged_count);
        fprintf(mf, "  \"artifacts\": [");
        for (int i = 0; i < manifest.num_required_artifacts; i++)
        {
            fprintf(mf, "\"%s\"%s", manifest.required_artifacts[i],
                    (i + 1 < manifest.num_required_artifacts) ? ", " : "");
        }
        fprintf(mf, "]\n");
        fprintf(mf, "}\n");
        fclose(mf);
    }

    // Phase 6: Clean up ephemeral sandbox directory (preserve persistent workspace)
    if (!manifest.is_workspace)
    {
        char rm_cmd[1024];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", sandbox_dir);
        if (system(rm_cmd) != 0) {
            // Log cleanup warning if needed
        }
    }

    // Read log snippet into TaskResultStore
    char log_snippet[MAX_TASK_OUTPUT] = {0};
    size_t snippet_len = 0;
    int rf = open(log_path, O_RDONLY);
    if (rf >= 0)
    {
        ssize_t n = read(rf, log_snippet, sizeof(log_snippet) - 1);
        if (n > 0)
        {
            snippet_len = (size_t)n;
            log_snippet[snippet_len] = '\0';
        }
        close(rf);
    }

    task_result_record_completed_full(task->taskId, exit_code, staged_count, log_snippet, snippet_len);
    return exit_code;
}

/* ---------------- Legacy Task Execution & Worker Pool ---------------- */

int execute_task(const Task *task, char *out_buf, size_t out_buf_size, size_t *out_len)
{
    if (out_len)
        *out_len = 0;

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
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
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
    if (out_len)
        *out_len = total;
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

static TaskQueue queue;

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
    if (queue.count <= NUM_WORKERS)
    {
        pthread_cond_signal(&queue.not_empty);
    }
    pthread_mutex_unlock(&queue.lock);
    return 0;
}

void *worker_thread(void *arg)
{
    (void)arg;
    char output[MAX_TASK_OUTPUT];
    Task batch[WORKER_BATCH];

    while (1)
    {
        pthread_mutex_lock(&queue.lock);
        while (queue.count == 0)
        {
            pthread_cond_wait(&queue.not_empty, &queue.lock);
        }

        int batch_size = 0;
        while (batch_size < WORKER_BATCH && queue.count > 0)
        {
            batch[batch_size++] = queue.tasks[queue.head];
            queue.head = (queue.head + 1) % MAX_QUEUE;
            queue.count--;
        }
        pthread_mutex_unlock(&queue.lock);

        for (int i = 0; i < batch_size; i++)
        {
            Task *t = &batch[i];

            if (t->type == TASK_TYPE_WORKFLOW)
            {
                task_result_record_running(t->taskId);
                execute_workflow_task(t);
                continue;
            }

            if (likely(t->taskCommand[0] == ':' && t->taskCommand[1] == '\0'))
            {
                continue;
            }

            task_result_record_running(t->taskId);

            output[0] = '\0';
            size_t out_len = 0;
            int exit_status = execute_task(t, output, sizeof(output), &out_len);

            task_result_record_completed(t->taskId, exit_status, output, out_len);
        }
    }
    return NULL;
}

int parse_task(const char *body, size_t body_len, Task *task)
{
    if (unlikely(body_len < sizeof(TaskHeader)))
        return -1;

    task->type = TASK_TYPE_COMMAND;
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

/* ---------------- Connection & HTTP Processing ---------------- */

typedef struct Connection
{
    int fd;
    char *input;
    size_t input_cap;
    size_t input_len;
    char output[OUTPUT_BUFFER_SIZE];
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

static const char response_retrieve_bad_request[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 15\r\n"
    "\r\n"
    "Missing task id";
static const size_t response_retrieve_bad_request_len = sizeof(response_retrieve_bad_request) - 1;

static const char response_retrieve_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 14\r\n"
    "\r\n"
    "Task Not Found";
static const size_t response_retrieve_404_len = sizeof(response_retrieve_404) - 1;

static const char response_retrieve_202[] =
    "HTTP/1.1 202 Accepted\r\n"
    "Content-Type: text/plain\r\n"
    "X-Task-Status: Running\r\n"
    "Content-Length: 12\r\n"
    "\r\n"
    "Task Running";
static const size_t response_retrieve_202_len = sizeof(response_retrieve_202) - 1;

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

#if defined(__APPLE__)
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
    if (conn->input)
        free(conn->input);
    free(conn);
}
#else
static inline int update_interest(int epfd, Connection *conn, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events | EPOLLRDHUP;
    ev.data.ptr = conn;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
}

static inline void close_connection(int epfd, Connection *conn)
{
    if (!conn)
        return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    if (conn->input)
        free(conn->input);
    free(conn);
}
#endif

static inline ssize_t complete_request_size(Connection *conn)
{
    conn->input[conn->input_len] = '\0';
    char *header_end = strstr(conn->input, "\r\n\r\n");
    if (!header_end)
        return 0;

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
    if (unlikely(total > MAX_PAYLOAD_SIZE))
        return -1;

    // Expand buffer if needed to accept full request
    if (conn->input_cap < total + 1024)
    {
        size_t new_cap = total + 1024;
        char *nb = realloc(conn->input, new_cap);
        if (!nb)
            return -1;
        conn->input = nb;
        conn->input_cap = new_cap;
    }

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
    if (len > sizeof(conn->output))
        len = sizeof(conn->output);
    conn->output_len = len;
    memcpy(conn->output, src, len);
    conn->output_offset = 0;
}

static void write_dynamic_response(Connection *conn, const char *headers, size_t hdr_len,
                                   const char *body, size_t body_len)
{
    if (hdr_len > sizeof(conn->output))
        hdr_len = sizeof(conn->output);
    memcpy(conn->output, headers, hdr_len);

    size_t max_body = sizeof(conn->output) - hdr_len;
    if (body && body_len > 0)
    {
        if (body_len > max_body)
            body_len = max_body;
        memcpy(conn->output + hdr_len, body, body_len);
    }
    else
    {
        body_len = 0;
    }
    conn->output_len = hdr_len + body_len;
    conn->output_offset = 0;
}

static bool extract_retrieve_id(const char *req, size_t req_len, char *task_id_out, size_t max_id_len)
{
    if (req_len < 14)
        return false;

    const char *p = req + 4; // Skip "GET "
    while (*p == ' ')
        p++;
    if (strncmp(p, "/retrieve", 9) != 0)
        return false;

    p += 9;
    if (*p == '/')
    {
        p++;
        size_t i = 0;
        while (*p && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n' && i < max_id_len - 1)
        {
            task_id_out[i++] = *p++;
        }
        task_id_out[i] = '\0';
        return i > 0;
    }
    else if (*p == '?')
    {
        p++;
        const char *line_end = strchr(p, '\r');
        if (!line_end)
            line_end = strchr(p, ' ');
        if (!line_end)
            line_end = req + req_len;

        const char *id_ptr = NULL;
        if (strncmp(p, "id=", 3) == 0)
        {
            id_ptr = p + 3;
        }
        else if (strncmp(p, "taskId=", 7) == 0)
        {
            id_ptr = p + 7;
        }
        else
        {
            const char *found = strstr(p, "&id=");
            if (found && found < line_end)
            {
                id_ptr = found + 4;
            }
            else
            {
                found = strstr(p, "&taskId=");
                if (found && found < line_end)
                {
                    id_ptr = found + 8;
                }
            }
        }

        if (id_ptr && id_ptr < line_end)
        {
            size_t i = 0;
            while (id_ptr < line_end && *id_ptr != '&' && *id_ptr != ' ' && *id_ptr != '\r' && *id_ptr != '\n' && i < max_id_len - 1)
            {
                task_id_out[i++] = *id_ptr++;
            }
            task_id_out[i] = '\0';
            return i > 0;
        }
    }
    return false;
}

static void send_json_response(Connection *conn, int code, const char *status_text, const char *body)
{
    size_t blen = body ? strlen(body) : 0;
    bool want_close = request_wants_close(conn->input, conn->header_bytes);
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, X-July-Token, Authorization\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        code, status_text, blen, want_close ? "close" : "keep-alive");
    write_dynamic_response(conn, hdr, (size_t)hlen, body ? body : "", blen);
}

static size_t json_escape_append(char *dst, size_t pos, size_t max, const char *src)
{
    if (!src) return pos;
    for (; *src && pos + 6 < max; src++)
    {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[pos++] = '\\'; dst[pos++] = (char)c; }
        else if (c == '\n') { dst[pos++] = '\\'; dst[pos++] = 'n'; }
        else if (c == '\r') { dst[pos++] = '\\'; dst[pos++] = 'r'; }
        else if (c == '\t') { dst[pos++] = '\\'; dst[pos++] = 't'; }
        else if (c >= 0x20) { dst[pos++] = (char)c; }
    }
    dst[pos] = '\0';
    return pos;
}

static void probe_ollama_status(bool *up_out, bool *loaded_out)
{
    *up_out = false;
    *loaded_out = false;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(11434);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    bool up = false;
    if (rc == 0) up = true;
    else if (errno == EINPROGRESS)
    {
        struct pollfd pfd = {.fd = s, .events = POLLOUT, .revents = 0};
        if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLOUT))
        {
            int so_err = 0;
            socklen_t sl = sizeof(so_err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &sl) == 0 && so_err == 0)
                up = true;
        }
    }
    if (!up)
    {
        close(s);
        return;
    }

    *up_out = true;
    const char req[] = "GET /api/ps HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    ssize_t sent = send(s, req, sizeof(req) - 1, 0);
    if (sent > 0)
    {
        char buf[1024];
        size_t tot = 0;
        for (int tries = 0; tries < 2 && tot + 1 < sizeof(buf); tries++)
        {
            struct pollfd rfd = {.fd = s, .events = POLLIN, .revents = 0};
            if (poll(&rfd, 1, 35) <= 0 || !(rfd.revents & POLLIN)) break;
            ssize_t n = recv(s, buf + tot, sizeof(buf) - 1 - tot, 0);
            if (n <= 0) break;
            tot += (size_t)n;
        }
        buf[tot] = '\0';
        if (strstr(buf, "\"models\":[{") != NULL || strstr(buf, "\"size\":") != NULL)
        {
            *loaded_out = true;
        }
    }
    close(s);
}

static void build_stats_json(char *out, size_t max_len)
{
    char hostname[128] = "july-node";
    gethostname(hostname, sizeof(hostname) - 1);
    struct utsname uts;
    const char *os_name = "linux";
    const char *arch_name = "x86_64";
    if (uname(&uts) == 0)
    {
        os_name = strcmp(uts.sysname, "Darwin") == 0 ? "darwin" : "linux";
        arch_name = uts.machine;
    }

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;

    double load1 = 0.0, load5 = 0.0;
#if defined(__linux__)
    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf)
    {
        if (fscanf(lf, "%lf %lf", &load1, &load5) < 1) load1 = 0.0;
        fclose(lf);
    }
#else
    double la[2] = {0.0, 0.0};
    if (getloadavg(la, 2) > 0) { load1 = la[0]; load5 = la[1]; }
#endif

    unsigned long long ram_total_mb = 0, ram_avail_mb = 0, ram_used_mb = 0;
#if defined(__linux__)
    FILE *mf = fopen("/proc/meminfo", "r");
    if (mf)
    {
        char k[64];
        unsigned long long v = 0;
        unsigned long long mt = 0, ma = 0;
        char line[128];
        while (fgets(line, sizeof(line), mf))
        {
            if (sscanf(line, "%63[^:]: %llu", k, &v) == 2)
            {
                if (strcmp(k, "MemTotal") == 0) mt = v / 1024ULL;
                else if (strcmp(k, "MemAvailable") == 0) ma = v / 1024ULL;
            }
        }
        fclose(mf);
        ram_total_mb = mt;
        ram_avail_mb = ma;
        ram_used_mb = (mt >= ma) ? (mt - ma) : 0;
    }
#elif defined(__APPLE__)
    uint64_t mem_bytes = 0;
    size_t mem_len = sizeof(mem_bytes);
    if (sysctlbyname("hw.memsize", &mem_bytes, &mem_len, NULL, 0) == 0)
    {
        ram_total_mb = (unsigned long long)(mem_bytes / (1024ULL * 1024ULL));
        vm_statistics64_data_t vmstat;
        mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
        vm_size_t page_size = 4096;
        host_page_size(mach_host_self(), &page_size);
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &vm_count) == KERN_SUCCESS)
        {
            unsigned long long free_bytes =
                ((unsigned long long)vmstat.free_count + (unsigned long long)vmstat.inactive_count) *
                (unsigned long long)page_size;
            ram_avail_mb = free_bytes / (1024ULL * 1024ULL);
            if (ram_avail_mb > ram_total_mb) ram_avail_mb = ram_total_mb;
            ram_used_mb = ram_total_mb - ram_avail_mb;
        }
    }
#endif

    unsigned long long tmpfs_total_mb = 0, tmpfs_used_mb = 0;
    struct statvfs vfs;
    if (statvfs("/tmp", &vfs) == 0 && vfs.f_blocks > 0)
    {
        unsigned long long tot = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
        unsigned long long avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
        tmpfs_total_mb = tot / (1024ULL * 1024ULL);
        tmpfs_used_mb = (tot >= avail) ? ((tot - avail) / (1024ULL * 1024ULL)) : 0;
    }

    pthread_mutex_lock(&queue.lock);
    int q_depth = queue.count;
    pthread_mutex_unlock(&queue.lock);

    int active_cnt = 0;
    pthread_mutex_lock(&g_recent_lock);
    int rcnt = g_recent_count;
    char ids[MAX_RECENT_TASKS][MAX_TASK_ID];
    for (int i = 0; i < rcnt; i++)
        memcpy(ids[i], g_recent_tasks[i], MAX_TASK_ID);
    pthread_mutex_unlock(&g_recent_lock);

    for (int i = 0; i < rcnt; i++)
    {
        unsigned int h = hash_task_id(ids[i]);
        ResultStripe *st = &g_stripes[h % NUM_STRIPES];
        unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;
        pthread_mutex_lock(&st->lock);
        int idx = st->buckets[b];
        while (idx != -1)
        {
            if (strncmp(st->entries[idx].taskId, ids[i], MAX_TASK_ID) == 0)
            {
                if (st->entries[idx].status == TASK_STATUS_RUNNING) active_cnt++;
                break;
            }
            idx = st->entries[idx].next;
        }
        pthread_mutex_unlock(&st->lock);
    }

    bool ollama_up = false;
    bool ollama_loaded = false;
    probe_ollama_status(&ollama_up, &ollama_loaded);
    long uptime_sec = (long)(time(NULL) - g_server_start_time);

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, max_len - pos,
        "{\"hostname\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\",\"cores\":%ld,"
        "\"load_1m\":%.2f,\"load_5m\":%.2f,"
        "\"ram_total_mb\":%llu,\"ram_used_mb\":%llu,\"ram_available_mb\":%llu,"
        "\"tmpfs_total_mb\":%llu,\"tmpfs_used_mb\":%llu,"
        "\"queue_depth\":%d,\"max_queue\":%d,\"active_tasks\":%d,"
        "\"uptime_sec\":%ld,\"ollama_up\":%s,\"ollama_loaded\":%s,\"workspaces\":[",
        hostname, os_name, arch_name, cores,
        load1, load5,
        ram_total_mb, ram_used_mb, ram_avail_mb,
        tmpfs_total_mb, tmpfs_used_mb,
        q_depth, MAX_QUEUE, active_cnt,
        uptime_sec, ollama_up ? "true" : "false", ollama_loaded ? "true" : "false");

    DIR *wd = opendir("/tmp/july_workspaces");
    if (wd)
    {
        struct dirent *ent;
        bool first = true;
        while ((ent = readdir(wd)) != NULL && pos + 64 < max_len)
        {
            if (ent->d_name[0] == '.') continue;
            if (!first) pos += (size_t)snprintf(out + pos, max_len - pos, ",");
            out[pos++] = '"';
            pos = json_escape_append(out, pos, max_len - 16, ent->d_name);
            out[pos++] = '"';
            first = false;
        }
        closedir(wd);
    }
    snprintf(out + pos, max_len - pos, "]}");
}

static const char *find_tmux_bin(void)
{
    if (access("/opt/homebrew/bin/tmux", X_OK) == 0) return "/opt/homebrew/bin/tmux";
    if (access("/usr/local/bin/tmux", X_OK) == 0) return "/usr/local/bin/tmux";
    if (access("/usr/bin/tmux", X_OK) == 0) return "/usr/bin/tmux";
    return "tmux";
}

static int run_exec_capture(const char *const argv[], char *out, size_t max_out)
{
    bool capture = (out != NULL && max_out > 1);
    if (out && max_out > 0) out[0] = '\0';

    int pfd[2] = {-1, -1};
    if (capture && pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        if (capture)
        {
            close(pfd[0]);
            close(pfd[1]);
        }
        return -1;
    }
    if (pid == 0)
    {
        int devnull = open("/dev/null", O_RDWR);
        if (capture)
        {
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        }
        else if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    if (!capture)
    {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    close(pfd[1]);
    int fl = fcntl(pfd[0], F_GETFL, 0);
    if (fl >= 0) fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);

    size_t pos = 0;
    bool exited = false;
    while (pos + 1 < max_out)
    {
        struct pollfd rfd = {.fd = pfd[0], .events = POLLIN, .revents = 0};
        int prc = poll(&rfd, 1, exited ? 15 : 1500);
        if (prc > 0 && (rfd.revents & (POLLIN | POLLHUP)))
        {
            ssize_t n = read(pfd[0], out + pos, max_out - 1 - pos);
            if (n > 0)
            {
                pos += (size_t)n;
                continue;
            }
            if (n == 0) break;
            if (errno != EAGAIN && errno != EINTR) break;
        }
        else if (prc == 0 && exited)
        {
            break;
        }
        else if (prc < 0 && errno != EINTR)
        {
            break;
        }

        if (!exited)
        {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) exited = true;
            else if (prc == 0) break;
        }
    }
    out[pos] = '\0';
    close(pfd[0]);

    if (!exited)
    {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void sanitize_tmux_name(const char *in, char *out, size_t out_max)
{
    size_t j = 0;
    if (in)
    {
        for (size_t i = 0; in[i] && j + 1 < out_max; i++)
        {
            char c = in[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')
            {
                out[j++] = c;
            }
        }
    }
    if (j == 0 && out_max > 5)
    {
        strcpy(out, "july");
        return;
    }
    out[j] = '\0';
}

static void extract_query_param(const char *req_line, const char *key, char *out, size_t out_max)
{
    out[0] = '\0';
    if (!req_line || !key) return;
    const char *q = strchr(req_line, '?');
    const char *sp = strpbrk(req_line + 4, " \r\n");
    if (!q || (sp && q > sp)) return;
    q++;
    size_t klen = strlen(key);
    while (q && (!sp || q < sp) && *q && *q != ' ' && *q != '\r' && *q != '\n')
    {
        const char *amp = strchr(q, '&');
        const char *end = (amp && (!sp || amp < sp)) ? amp : sp;
        if (!end) end = q + strlen(q);
        if ((size_t)(end - q) > klen && strncmp(q, key, klen) == 0 && q[klen] == '=')
        {
            const char *val = q + klen + 1;
            size_t vlen = (size_t)(end - val);
            if (vlen >= out_max) vlen = out_max - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return;
        }
        if (!amp || (sp && amp >= sp)) break;
        q = amp + 1;
    }
}

static bool tmux_ensure_session(const char *tmux_bin, const char *sess, const char *cwd)
{
    const char *has_argv[] = {tmux_bin, "has-session", "-t", sess, NULL};
    if (run_exec_capture(has_argv, NULL, 0) == 0) return true;

    const char *start_dir = NULL;
    if (cwd && cwd[0] && access(cwd, F_OK) == 0) start_dir = cwd;
    else
    {
        start_dir = getenv("HOME");
        if (!start_dir || access(start_dir, F_OK) != 0) start_dir = "/tmp";
    }
    const char *new_argv[] = {tmux_bin, "new-session", "-d", "-s", sess, "-c", start_dir, NULL};
    if (run_exec_capture(new_argv, NULL, 0) != 0) return false;

    const char *hist_argv[] = {tmux_bin, "set-option", "-t", sess, "history-limit", "5000", NULL};
    run_exec_capture(hist_argv, NULL, 0);
    usleep(60000);
    return true;
}

static void tmux_get_meta(const char *tmux_bin, const char *sess, char *cwd_out, size_t cwd_max, char *proc_out, size_t proc_max)
{
    cwd_out[0] = '\0';
    proc_out[0] = '\0';
    char meta[768] = {0};
    const char *argv[] = {tmux_bin, "display-message", "-t", sess, "-p", "#{pane_current_path}\t#{pane_current_command}", NULL};
    if (run_exec_capture(argv, meta, sizeof(meta)) == 0)
    {
        char *nl = strpbrk(meta, "\r\n");
        if (nl) *nl = '\0';
        char *tab = strchr(meta, '\t');
        if (tab)
        {
            *tab = '\0';
            safe_copy_str(cwd_out, cwd_max, meta);
            safe_copy_str(proc_out, proc_max, tab + 1);
        }
        else
        {
            safe_copy_str(cwd_out, cwd_max, meta);
        }
    }
}

static bool is_allowed_tmux_key(const char *k)
{
    static const char *const allowed[] = {
        "C-c", "C-d", "C-z", "C-l", "Enter", "Escape", "Tab", "Up", "Down", "BSpace"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
    {
        if (strcmp(k, allowed[i]) == 0) return true;
    }
    return false;
}

static void build_tasks_json(char *out, size_t max_len)
{
    pthread_mutex_lock(&g_recent_lock);
    int cnt = g_recent_count;
    int head = g_recent_head;
    char ids[MAX_RECENT_TASKS][MAX_TASK_ID];
    for (int i = 0; i < cnt; i++)
    {
        int ring_idx = (head - 1 - i + MAX_RECENT_TASKS) % MAX_RECENT_TASKS;
        memcpy(ids[i], g_recent_tasks[ring_idx], MAX_TASK_ID);
    }
    pthread_mutex_unlock(&g_recent_lock);

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, max_len - pos, "{\"tasks\":[");
    bool first = true;
    int limit = cnt < 32 ? cnt : 32;
    time_t now = time(NULL);

    for (int i = 0; i < limit && pos + 256 < max_len; i++)
    {
        unsigned int h = hash_task_id(ids[i]);
        ResultStripe *st = &g_stripes[h % NUM_STRIPES];
        unsigned int b = (h >> 6) % ENTRIES_PER_STRIPE;

        TaskResult snap = {0};
        bool found = false;
        pthread_mutex_lock(&st->lock);
        int idx = st->buckets[b];
        while (idx != -1)
        {
            if (strncmp(st->entries[idx].taskId, ids[i], MAX_TASK_ID) == 0)
            {
                snap = st->entries[idx];
                found = true;
                break;
            }
            idx = st->entries[idx].next;
        }
        pthread_mutex_unlock(&st->lock);

        if (!found) continue;

        const char *st_str = "queued";
        if (snap.status == TASK_STATUS_RUNNING) st_str = "running";
        else if (snap.status == TASK_STATUS_COMPLETED) st_str = "completed";

        long elapsed_s = 0;
        if (snap.created_at > 0)
        {
            time_t end_t = snap.finished_at > 0 ? snap.finished_at : now;
            elapsed_s = (long)(end_t >= snap.created_at ? (end_t - snap.created_at) : 0);
        }

        size_t log_bytes = snap.output_len;
        char lp[256];
        snprintf(lp, sizeof(lp), "/tmp/july_storage/%s/stdout.log", snap.taskId);
        struct stat lst;
        if (stat(lp, &lst) == 0) log_bytes = (size_t)lst.st_size;

        if (!first) out[pos++] = ',';
        pos += (size_t)snprintf(out + pos, max_len - pos,
            "{\"task_id\":\"%s\",\"label\":\"", snap.taskId);
        pos = json_escape_append(out, pos, max_len - 180, snap.label[0] ? snap.label : snap.taskId);
        pos += (size_t)snprintf(out + pos, max_len - pos,
            "\",\"type\":\"%s\",\"status\":\"%s\",\"exit_code\":%d,"
            "\"artifacts_count\":%d,\"created_at\":%ld,\"elapsed_sec\":%ld,"
            "\"log_bytes\":%zu,\"killed\":%s}",
            snap.is_workflow ? "workflow" : "command",
            st_str, snap.exit_code, snap.artifacts_count,
            (long)snap.created_at, elapsed_s, log_bytes,
            snap.killed ? "true" : "false");
        first = false;
    }

    snprintf(out + pos, max_len - pos, "]}");
}

static bool extract_json_str_field(const char *json, size_t len, const char *target_key, char *out, size_t out_max)
{
    out[0] = '\0';
    const char *end = json + len;
    const char *p = skip_json_ws(json, end);
    if (p >= end || *p != '{') return false;
    p++;
    while (p < end)
    {
        p = skip_json_ws(p, end);
        if (p < end && *p == '}') break;
        char key[64];
        if (!parse_json_string(&p, end, key, sizeof(key))) break;
        p = skip_json_ws(p, end);
        if (p >= end || *p != ':') break;
        p++;
        p = skip_json_ws(p, end);
        if (strcmp(key, target_key) == 0 && p < end && *p == '"')
        {
            return parse_json_string(&p, end, out, out_max);
        }
        p = skip_json_value(p, end);
        p = skip_json_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return false;
}

static int prepare_response(Connection *conn, size_t request_len)
{
    conn->input[request_len] = '\0';
    conn->close_after_write = request_wants_close(conn->input, conn->header_bytes);

    if (strncmp(conn->input, "OPTIONS ", 8) == 0)
    {
        const char *opt_resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-July-Token, Authorization\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        write_response(conn, opt_resp, strlen(opt_resp));
        conn->input_len -= request_len;
        if (conn->input_len > 0)
            memmove(conn->input, conn->input + request_len, conn->input_len);
        conn->input[conn->input_len] = '\0';
        return 0;
    }

    // Authentication enforcement
    if (!is_request_authenticated(conn->input, conn->header_bytes))
    {
        write_response(conn, response_401, response_401_len);
        conn->input_len -= request_len;
        if (conn->input_len > 0)
        {
            memmove(conn->input, conn->input + request_len, conn->input_len);
        }
        conn->input[conn->input_len] = '\0';
        return 0;
    }

    if (conn->input[0] == 'G' && strncmp(conn->input, "GET ", 4) == 0)
    {
        if (strncmp(conn->input + 4, "/ ", 2) == 0)
        {
            write_response(conn, response_200, response_200_len);
        }
        else if (strncmp(conn->input + 4, "/stats ", 7) == 0 || strncmp(conn->input + 4, "/stats?", 7) == 0)
        {
            char stats_buf[OUTPUT_BUFFER_SIZE - 512];
            build_stats_json(stats_buf, sizeof(stats_buf));
            send_json_response(conn, 200, "OK", stats_buf);
        }
        else if (strncmp(conn->input + 4, "/tasks ", 7) == 0 || strncmp(conn->input + 4, "/tasks?", 7) == 0)
        {
            char tasks_buf[OUTPUT_BUFFER_SIZE - 512];
            build_tasks_json(tasks_buf, sizeof(tasks_buf));
            send_json_response(conn, 200, "OK", tasks_buf);
        }
        else if (strncmp(conn->input + 4, "/tmux/sessions ", 15) == 0 || strncmp(conn->input + 4, "/tmux/sessions?", 15) == 0)
        {
            const char *tmux_bin = find_tmux_bin();
            const char *v_argv[] = {tmux_bin, "-V", NULL};
            char ver_buf[128] = {0};
            if (run_exec_capture(v_argv, ver_buf, sizeof(ver_buf)) != 0)
            {
                send_json_response(conn, 200, "OK", "{\"available\":false,\"sessions\":[]}");
            }
            else
            {
                char ls_buf[4096] = {0};
                const char *ls_argv[] = {
                    tmux_bin, "list-sessions", "-F",
                    "#{session_name}\t#{session_windows}\t#{session_attached}\t#{pane_current_path}\t#{pane_current_command}",
                    NULL
                };
                int rc = run_exec_capture(ls_argv, ls_buf, sizeof(ls_buf));
                char resp[OUTPUT_BUFFER_SIZE];
                size_t pos = 0;
                pos += (size_t)snprintf(resp + pos, sizeof(resp) - pos, "{\"available\":true,\"sessions\":[");
                if (rc == 0)
                {
                    bool first = true;
                    char *line = ls_buf;
                    while (line && *line && pos + 256 < sizeof(resp))
                    {
                        char *nl = strpbrk(line, "\r\n");
                        if (nl) *nl++ = '\0';
                        char *t1 = strchr(line, '\t');
                        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
                        char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
                        char *t4 = t3 ? strchr(t3 + 1, '\t') : NULL;
                        if (t1 && t2 && t3 && t4)
                        {
                            *t1 = '\0'; *t2 = '\0'; *t3 = '\0'; *t4 = '\0';
                            if (!first) resp[pos++] = ',';
                            pos += (size_t)snprintf(resp + pos, sizeof(resp) - pos, "{\"name\":\"");
                            pos = json_escape_append(resp, pos, sizeof(resp) - 128, line);
                            pos += (size_t)snprintf(resp + pos, sizeof(resp) - pos, "\",\"windows\":%d,\"attached\":%d,\"cwd\":\"", atoi(t1 + 1), atoi(t2 + 1));
                            pos = json_escape_append(resp, pos, sizeof(resp) - 96, t3 + 1);
                            pos += (size_t)snprintf(resp + pos, sizeof(resp) - pos, "\",\"process\":\"");
                            pos = json_escape_append(resp, pos, sizeof(resp) - 32, t4 + 1);
                            pos += (size_t)snprintf(resp + pos, sizeof(resp) - pos, "\"}");
                            first = false;
                        }
                        line = nl ? nl : NULL;
                    }
                }
                snprintf(resp + pos, sizeof(resp) - pos, "]}");
                send_json_response(conn, 200, "OK", resp);
            }
        }
        else if (strncmp(conn->input + 4, "/tmux/pane ", 11) == 0 || strncmp(conn->input + 4, "/tmux/pane?", 11) == 0)
        {
            const char *tmux_bin = find_tmux_bin();
            char raw_sess[64] = {0}, auto_str[16] = {0}, lines_str[16] = {0};
            extract_query_param(conn->input, "session", raw_sess, sizeof(raw_sess));
            extract_query_param(conn->input, "autostart", auto_str, sizeof(auto_str));
            extract_query_param(conn->input, "lines", lines_str, sizeof(lines_str));

            char sess[64];
            sanitize_tmux_name(raw_sess, sess, sizeof(sess));
            int nlines = atoi(lines_str);
            if (nlines <= 0 || nlines > 400) nlines = 150;
            char start_arg[24];
            snprintf(start_arg, sizeof(start_arg), "-%d", nlines);

            const char *has_argv[] = {tmux_bin, "has-session", "-t", sess, NULL};
            bool exists = (run_exec_capture(has_argv, NULL, 0) == 0);
            if (!exists && strcmp(auto_str, "1") == 0)
            {
                exists = tmux_ensure_session(tmux_bin, sess, NULL);
            }

            if (!exists)
            {
                char resp[256];
                snprintf(resp, sizeof(resp), "{\"exists\":false,\"session\":\"%s\",\"cwd\":\"\",\"process\":\"\",\"content\":\"\"}", sess);
                send_json_response(conn, 200, "OK", resp);
            }
            else
            {
                char cwd[512] = {0}, proc[128] = {0};
                tmux_get_meta(tmux_bin, sess, cwd, sizeof(cwd), proc, sizeof(proc));

                size_t raw_cap = 16384;
                char *raw_buf = malloc(raw_cap);
                if (!raw_buf)
                {
                    send_json_response(conn, 500, "Internal Server Error", "{\"error\":\"oom\"}");
                }
                else
                {
                    const char *cap_argv[] = {tmux_bin, "capture-pane", "-p", "-J", "-S", start_arg, "-t", sess, NULL};
                    run_exec_capture(cap_argv, raw_buf, raw_cap);
                    size_t rlen = strlen(raw_buf);
                    while (rlen > 0 && (raw_buf[rlen - 1] == '\n' || raw_buf[rlen - 1] == '\r'))
                    {
                        raw_buf[--rlen] = '\0';
                    }

                    size_t out_cap = rlen * 2 + 2048;
                    char *out_buf = malloc(out_cap);
                    if (!out_buf)
                    {
                        free(raw_buf);
                        send_json_response(conn, 500, "Internal Server Error", "{\"error\":\"oom\"}");
                    }
                    else
                    {
                        size_t pos = 0;
                        pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "{\"exists\":true,\"session\":\"%s\",\"cwd\":\"", sess);
                        pos = json_escape_append(out_buf, pos, out_cap - 512, cwd);
                        pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "\",\"process\":\"");
                        pos = json_escape_append(out_buf, pos, out_cap - 256, proc);
                        pos += (size_t)snprintf(out_buf + pos, out_cap - pos, "\",\"content\":\"");
                        pos = json_escape_append(out_buf, pos, out_cap - 16, raw_buf);
                        snprintf(out_buf + pos, out_cap - pos, "\"}");
                        send_json_response(conn, 200, "OK", out_buf);
                        free(out_buf);
                        free(raw_buf);
                    }
                }
            }
        }
        else if (strncmp(conn->input + 4, "/stream/", 8) == 0)
        {
            // Live incremental byte-offset log stream: GET /stream/<taskId>?offset=<N>
            const char *id_start = conn->input + 12;
            const char *id_end = id_start;
            while (*id_end && *id_end != ' ' && *id_end != '?' && *id_end != '\r' && *id_end != '\n')
                id_end++;
            size_t id_len = (size_t)(id_end - id_start);
            char task_id[MAX_TASK_ID] = {0};
            if (id_len >= sizeof(task_id)) id_len = sizeof(task_id) - 1;
            memcpy(task_id, id_start, id_len);
            task_id[id_len] = '\0';

            size_t req_offset = 0;
            if (*id_end == '?')
            {
                const char *q = id_end + 1;
                const char *q_end = q;
                while (*q_end && *q_end != ' ' && *q_end != '\r' && *q_end != '\n') q_end++;
                const char *off_p = strstr(q, "offset=");
                if (off_p && off_p < q_end)
                {
                    req_offset = (size_t)strtoull(off_p + 7, NULL, 10);
                }
            }

            if (task_id[0] == '\0' || strstr(task_id, "..") || strchr(task_id, '/'))
            {
                write_response(conn, response_retrieve_bad_request, response_retrieve_bad_request_len);
            }
            else
            {
                char storage_log[512], storage_meta[512];
                snprintf(storage_log, sizeof(storage_log), "/tmp/july_storage/%s/stdout.log", task_id);
                snprintf(storage_meta, sizeof(storage_meta), "/tmp/july_storage/%s/meta.json", task_id);

                struct stat meta_st;
                bool completed = (stat(storage_meta, &meta_st) == 0);
                int exit_code = 0;
                int art_count = 0;
                if (completed)
                {
                    FILE *mf = fopen(storage_meta, "r");
                    if (mf)
                    {
                        char mline[256];
                        while (fgets(mline, sizeof(mline), mf))
                        {
                            if (strstr(mline, "\"exit_code\":"))
                                sscanf(strstr(mline, "\"exit_code\":") + 12, "%d", &exit_code);
                            if (strstr(mline, "\"artifacts_count\":"))
                                sscanf(strstr(mline, "\"artifacts_count\":") + 18, "%d", &art_count);
                        }
                        fclose(mf);
                    }
                }

                int log_fd = open(storage_log, O_RDONLY);
                if (log_fd >= 0)
                {
                    struct stat log_st;
                    fstat(log_fd, &log_st);
                    size_t file_size = (size_t)log_st.st_size;
                    if (req_offset > file_size) req_offset = file_size;
                    size_t delta = file_size - req_offset;
                    size_t new_offset = req_offset + delta;
                    bool want_close = request_wants_close(conn->input, conn->header_bytes);

                    if (!completed && delta == 0)
                    {
                        close(log_fd);
                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 202 Accepted\r\n"
                            "Content-Type: text/plain\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Expose-Headers: X-Task-Status, X-Log-Offset, X-Task-Exit-Code, X-Artifacts-Count\r\n"
                            "X-Task-Status: Running\r\n"
                            "X-Log-Offset: %zu\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            new_offset, want_close ? "close" : "keep-alive");
                        write_dynamic_response(conn, hdr, (size_t)hlen, "", 0);
                    }
                    else if (delta <= sizeof(conn->output) - 512)
                    {
                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Expose-Headers: X-Task-Status, X-Log-Offset, X-Task-Exit-Code, X-Artifacts-Count\r\n"
                            "X-Task-Status: %s\r\n"
                            "X-Log-Offset: %zu\r\n"
                            "X-Task-Exit-Code: %d\r\n"
                            "X-Artifacts-Count: %d\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            completed ? "Completed" : "Running",
                            new_offset, exit_code, art_count, delta,
                            want_close ? "close" : "keep-alive");
                        char slice_buf[OUTPUT_BUFFER_SIZE];
                        size_t got = 0;
                        if (delta > 0 && lseek(log_fd, (off_t)req_offset, SEEK_SET) >= 0)
                        {
                            while (got < delta)
                            {
                                ssize_t rn = read(log_fd, slice_buf + got, delta - got);
                                if (rn <= 0) break;
                                got += (size_t)rn;
                            }
                        }
                        close(log_fd);
                        write_dynamic_response(conn, hdr, (size_t)hlen, slice_buf, got);
                    }
                    else
                    {
                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Expose-Headers: X-Task-Status, X-Log-Offset, X-Task-Exit-Code, X-Artifacts-Count\r\n"
                            "X-Task-Status: %s\r\n"
                            "X-Log-Offset: %zu\r\n"
                            "X-Task-Exit-Code: %d\r\n"
                            "X-Artifacts-Count: %d\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            completed ? "Completed" : "Running",
                            new_offset, exit_code, art_count, delta);
#if defined(__linux__)
                        int cork = 1;
                        setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
#if defined(MSG_NOSIGNAL)
                        send(conn->fd, hdr, hlen, MSG_NOSIGNAL);
#else
                        send(conn->fd, hdr, hlen, 0);
#endif
                        send_file_slice_zero_copy(conn->fd, log_fd, (off_t)req_offset, delta);
                        close(log_fd);
#if defined(__linux__)
                        cork = 0;
                        setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
                        conn->close_after_write = true;
                        conn->output_len = 0;
                        conn->output_offset = 0;
                        return 1;
                    }
                }
                else
                {
                    TaskStatus status = 0;
                    char task_out[MAX_TASK_OUTPUT];
                    size_t task_out_len = 0;
                    if (task_result_get(task_id, &status, &exit_code, &art_count, task_out, &task_out_len, sizeof(task_out)) != 0)
                    {
                        write_response(conn, response_retrieve_404, response_retrieve_404_len);
                    }
                    else if (status != TASK_STATUS_COMPLETED)
                    {
                        char hdr[256];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 202 Accepted\r\n"
                            "Content-Type: text/plain\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Access-Control-Expose-Headers: X-Task-Status, X-Log-Offset, X-Task-Exit-Code, X-Artifacts-Count\r\n"
                            "X-Task-Status: Running\r\n"
                            "X-Log-Offset: %zu\r\n"
                            "Content-Length: 0\r\n"
                            "\r\n", req_offset);
                        write_dynamic_response(conn, hdr, (size_t)hlen, "", 0);
                    }
                    else
                    {
                        if (req_offset > task_out_len) req_offset = task_out_len;
                        size_t delta = task_out_len - req_offset;
                        char hdr[256];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "X-Task-Status: Completed\r\n"
                            "X-Log-Offset: %zu\r\n"
                            "X-Task-Exit-Code: %d\r\n"
                            "X-Artifacts-Count: %d\r\n"
                            "Content-Length: %zu\r\n"
                            "\r\n",
                            task_out_len, exit_code, art_count, delta);
                        write_dynamic_response(conn, hdr, (size_t)hlen, task_out + req_offset, delta);
                    }
                }
            }
        }
        else if (strncmp(conn->input + 4, "/retrieve/", 10) == 0 && strstr(conn->input + 14, "/meta"))
        {
            const char *id_start = conn->input + 14;
            const char *meta_marker = strstr(id_start, "/meta");
            size_t id_len = (size_t)(meta_marker - id_start);
            char task_id[MAX_TASK_ID] = {0};
            if (id_len >= sizeof(task_id)) id_len = sizeof(task_id) - 1;
            memcpy(task_id, id_start, id_len);
            task_id[id_len] = '\0';

            if (task_id[0] == '\0' || strstr(task_id, "..") || strchr(task_id, '/'))
            {
                write_response(conn, response_400, response_400_len);
            }
            else
            {
                char meta_path[512];
                snprintf(meta_path, sizeof(meta_path), "/tmp/july_storage/%s/meta.json", task_id);
                int mfd = open(meta_path, O_RDONLY);
                if (mfd < 0)
                {
                    write_response(conn, response_retrieve_404, response_retrieve_404_len);
                }
                else
                {
                    struct stat st;
                    fstat(mfd, &st);
                    bool want_close = request_wants_close(conn->input, conn->header_bytes);
                    char hdr[512];
                    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        (size_t)st.st_size, want_close ? "close" : "keep-alive");
#if defined(MSG_NOSIGNAL)
                    send(conn->fd, hdr, hlen, MSG_NOSIGNAL);
#else
                    send(conn->fd, hdr, hlen, 0);
#endif
                    send_file_zero_copy(conn->fd, mfd, (size_t)st.st_size);
                    close(mfd);
                    conn->close_after_write = want_close;
                    conn->output_len = 0;
                    conn->output_offset = 0;
                    return 1;
                }
            }
        }
        else if (strncmp(conn->input + 4, "/retrieve/", 10) == 0 && strstr(conn->input + 14, "/artifact/"))
        {
            // Zero-copy artifact download endpoint: GET /retrieve/<taskId>/artifact/<filename>
            const char *id_start = conn->input + 14;
            const char *art_marker = strstr(id_start, "/artifact/");
            size_t id_len = (size_t)(art_marker - id_start);
            char task_id[MAX_TASK_ID] = {0};
            if (id_len >= sizeof(task_id))
                id_len = sizeof(task_id) - 1;
            memcpy(task_id, id_start, id_len);
            task_id[id_len] = '\0';

            const char *fn_start = art_marker + 10;
            const char *fn_end = fn_start;
            while (*fn_end && *fn_end != ' ' && *fn_end != '?' && *fn_end != '\r' && *fn_end != '\n')
                fn_end++;
            size_t fn_len = (size_t)(fn_end - fn_start);
            char filename[256] = {0};
            if (fn_len >= sizeof(filename))
                fn_len = sizeof(filename) - 1;
            memcpy(filename, fn_start, fn_len);
            filename[fn_len] = '\0';

            if (filename[0] == '/' || strstr(filename, "..") || filename[0] == '\0')
            {
                write_response(conn, response_400, response_400_len);
            }
            else
            {
                char art_path[512];
                snprintf(art_path, sizeof(art_path), "/tmp/july_storage/%s/artifacts/%s", task_id, filename);
                int art_fd = open(art_path, O_RDONLY);
                if (art_fd < 0)
                {
                    write_response(conn, response_retrieve_404, response_retrieve_404_len);
                }
                else
                {
                    struct stat st;
                    fstat(art_fd, &st);
                    bool want_close = request_wants_close(conn->input, conn->header_bytes);
                    const char *mime = "application/octet-stream";
                    const char *disp = "attachment";
                    size_t fnl = strlen(filename);
                    if (fnl >= 4 && strcmp(filename + fnl - 4, ".png") == 0) { mime = "image/png"; disp = "inline"; }
                    else if (fnl >= 5 && strcmp(filename + fnl - 5, ".json") == 0) { mime = "application/json"; disp = "inline"; }
                    else if ((fnl >= 5 && strcmp(filename + fnl - 5, ".diff") == 0) ||
                             (fnl >= 4 && strcmp(filename + fnl - 4, ".log") == 0) ||
                             (fnl >= 4 && strcmp(filename + fnl - 4, ".txt") == 0)) { mime = "text/plain"; disp = "inline"; }
                    char hdr[512];
                    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: %s\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: %zu\r\n"
                        "Content-Disposition: %s; filename=\"%s\"\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        mime, (size_t)st.st_size,
                        disp,
                        filename, want_close ? "close" : "keep-alive");

#if defined(__linux__)
                    int cork = 1;
                    setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif

#if defined(MSG_NOSIGNAL)
                    send(conn->fd, hdr, hlen, MSG_NOSIGNAL);
#else
                    send(conn->fd, hdr, hlen, 0);
#endif
                    send_file_zero_copy(conn->fd, art_fd, (size_t)st.st_size);
                    close(art_fd);

#if defined(__linux__)
                    cork = 0;
                    setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
                    conn->close_after_write = want_close;
                    conn->output_len = 0;
                    conn->output_offset = 0;
                    return 1; // Streamed directly
                }
            }
        }
        else if (strncmp(conn->input + 4, "/retrieve", 9) == 0)
        {
            char task_id[MAX_TASK_ID] = {0};
            if (!extract_retrieve_id(conn->input, request_len, task_id, sizeof(task_id)))
            {
                write_response(conn, response_retrieve_bad_request, response_retrieve_bad_request_len);
            }
            else
            {
                char storage_log[512], storage_meta[512];
                snprintf(storage_log, sizeof(storage_log), "/tmp/july_storage/%s/stdout.log", task_id);
                snprintf(storage_meta, sizeof(storage_meta), "/tmp/july_storage/%s/meta.json", task_id);

                struct stat meta_st;
                if (stat(storage_meta, &meta_st) == 0)
                {
                    // Workflow task completed: parse meta.json for exit_code and artifacts_count
                    int exit_code = 0;
                    int art_count = 0;
                    FILE *mf = fopen(storage_meta, "r");
                    if (mf)
                    {
                        char mline[256];
                        while (fgets(mline, sizeof(mline), mf))
                        {
                            if (strstr(mline, "\"exit_code\":"))
                            {
                                sscanf(strstr(mline, "\"exit_code\":") + 12, "%d", &exit_code);
                            }
                            if (strstr(mline, "\"artifacts_count\":"))
                            {
                                sscanf(strstr(mline, "\"artifacts_count\":") + 18, "%d", &art_count);
                            }
                        }
                        fclose(mf);
                    }

                    int log_fd = open(storage_log, O_RDONLY);
                    if (log_fd >= 0)
                    {
                        struct stat log_st;
                        fstat(log_fd, &log_st);
                        bool want_close = request_wants_close(conn->input, conn->header_bytes);
                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "X-Task-Status: Completed\r\n"
                            "X-Task-Exit-Code: %d\r\n"
                            "X-Artifacts-Count: %d\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            exit_code, art_count, (size_t)log_st.st_size,
                            want_close ? "close" : "keep-alive");

#if defined(__linux__)
                        int cork = 1;
                        setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif

#if defined(MSG_NOSIGNAL)
                        send(conn->fd, hdr, hlen, MSG_NOSIGNAL);
#else
                        send(conn->fd, hdr, hlen, 0);
#endif
                        send_file_zero_copy(conn->fd, log_fd, (size_t)log_st.st_size);
                        close(log_fd);

#if defined(__linux__)
                        cork = 0;
                        setsockopt(conn->fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
                        conn->close_after_write = want_close;
                        conn->output_len = 0;
                        conn->output_offset = 0;
                        return 1;
                    }
                }

                // Check in-memory result store (either running or legacy task)
                TaskStatus status = 0;
                int exit_code = 0;
                int art_count = 0;
                char task_out[MAX_TASK_OUTPUT];
                size_t task_out_len = 0;

                if (task_result_get(task_id, &status, &exit_code, &art_count, task_out, &task_out_len, sizeof(task_out)) != 0)
                {
                    write_response(conn, response_retrieve_404, response_retrieve_404_len);
                }
                else if (status != TASK_STATUS_COMPLETED)
                {
                    write_response(conn, response_retrieve_202, response_retrieve_202_len);
                }
                else
                {
                    char hdr[256];
                    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "X-Task-Status: Completed\r\n"
                        "X-Task-Exit-Code: %d\r\n"
                        "X-Artifacts-Count: %d\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n",
                        exit_code, art_count, task_out_len);
                    write_dynamic_response(conn, hdr, (size_t)hlen, task_out, task_out_len);
                }
            }
        }
        else
        {
            write_response(conn, response_404, response_404_len);
        }
    }
    else if (strncmp(conn->input, "POST /tasks/", 12) == 0 && strstr(conn->input + 12, "/kill"))
    {
        const char *id_start = conn->input + 12;
        const char *km = strstr(id_start, "/kill");
        size_t id_len = (size_t)(km - id_start);
        char task_id[MAX_TASK_ID] = {0};
        if (id_len >= sizeof(task_id)) id_len = sizeof(task_id) - 1;
        memcpy(task_id, id_start, id_len);
        task_id[id_len] = '\0';

        bool killed = task_result_kill(task_id);
        char jbuf[256];
        snprintf(jbuf, sizeof(jbuf), "{\"task_id\":\"%s\",\"killed\":%s}", task_id, killed ? "true" : "false");
        send_json_response(conn, 200, "OK", jbuf);
    }
    else if (strncmp(conn->input, "POST /api/ollama/wake ", 22) == 0)
    {
        Task task = {0};
        task.type = TASK_TYPE_COMMAND;
        snprintf(task.taskId, sizeof(task.taskId), "cmd_%ld_%d", (long)time(NULL), rand() % 100000);
        snprintf(task.taskCommand, sizeof(task.taskCommand),
                 "(curl -sf http://127.0.0.1:11434/api/tags >/dev/null || "
                 "(nohup ollama serve >/tmp/july_ollama.log 2>&1 </dev/null & "
                 "for i in $(seq 1 30); do curl -sf http://127.0.0.1:11434/api/tags >/dev/null && break; sleep 0.2; done)) "
                 "&& curl -sf http://127.0.0.1:11434/api/generate -d '{\"model\":\"qwen2.5-coder:7b\",\"prompt\":\"\",\"keep_alive\":\"10m\"}'");
        if (enqueue_task(&task) == 0)
        {
            task_result_record_enqueued(task.taskId);
            task_result_set_meta(task.taskId, "ollama:wake", false);
            char jbuf[128];
            snprintf(jbuf, sizeof(jbuf), "{\"task_id\":\"%s\",\"status\":\"queued\"}", task.taskId);
            send_json_response(conn, 202, "Accepted", jbuf);
        }
        else
        {
            send_json_response(conn, 503, "Service Unavailable", "{\"error\":\"queue full\"}");
        }
    }
    else if (strncmp(conn->input, "POST /api/ollama/unload ", 24) == 0)
    {
        Task task = {0};
        task.type = TASK_TYPE_COMMAND;
        snprintf(task.taskId, sizeof(task.taskId), "cmd_%ld_%d", (long)time(NULL), rand() % 100000);
        snprintf(task.taskCommand, sizeof(task.taskCommand),
                 "curl -s http://127.0.0.1:11434/api/generate -d '{\"model\":\"qwen2.5-coder:7b\",\"keep_alive\":0}'");
        if (enqueue_task(&task) == 0)
        {
            task_result_record_enqueued(task.taskId);
            task_result_set_meta(task.taskId, "ollama:unload", false);
            char jbuf[128];
            snprintf(jbuf, sizeof(jbuf), "{\"task_id\":\"%s\",\"status\":\"queued\"}", task.taskId);
            send_json_response(conn, 202, "Accepted", jbuf);
        }
        else
        {
            send_json_response(conn, 503, "Service Unavailable", "{\"error\":\"queue full\"}");
        }
    }
    else if (strncmp(conn->input, "POST /tmux/exec ", 16) == 0)
    {
        const char *body = conn->input + conn->header_bytes;
        size_t body_len = request_len - conn->header_bytes;
        char raw_sess[64] = {0}, command[2048] = {0}, cwd[512] = {0};
        extract_json_str_field(body, body_len, "session", raw_sess, sizeof(raw_sess));
        extract_json_str_field(body, body_len, "command", command, sizeof(command));
        extract_json_str_field(body, body_len, "cwd", cwd, sizeof(cwd));

        char sess[64];
        sanitize_tmux_name(raw_sess, sess, sizeof(sess));
        const char *tmux_bin = find_tmux_bin();

        if (!tmux_ensure_session(tmux_bin, sess, cwd))
        {
            send_json_response(conn, 500, "Internal Server Error", "{\"error\":\"tmux unavailable or failed to create session\"}");
        }
        else
        {
            if (command[0] != '\0')
            {
                const char *send_argv[] = {tmux_bin, "send-keys", "-t", sess, "-l", "--", command, NULL};
                run_exec_capture(send_argv, NULL, 0);
                const char *ent_argv[] = {tmux_bin, "send-keys", "-t", sess, "Enter", NULL};
                run_exec_capture(ent_argv, NULL, 0);
                usleep(30000);
            }
            char cur_cwd[512] = {0}, cur_proc[128] = {0};
            tmux_get_meta(tmux_bin, sess, cur_cwd, sizeof(cur_cwd), cur_proc, sizeof(cur_proc));

            char jbuf[1024];
            size_t pos = 0;
            pos += (size_t)snprintf(jbuf + pos, sizeof(jbuf) - pos, "{\"ok\":true,\"session\":\"%s\",\"cwd\":\"", sess);
            pos = json_escape_append(jbuf, pos, sizeof(jbuf) - 256, cur_cwd);
            pos += (size_t)snprintf(jbuf + pos, sizeof(jbuf) - pos, "\",\"process\":\"");
            pos = json_escape_append(jbuf, pos, sizeof(jbuf) - 16, cur_proc);
            snprintf(jbuf + pos, sizeof(jbuf) - pos, "\"}");
            send_json_response(conn, 200, "OK", jbuf);
        }
    }
    else if (strncmp(conn->input, "POST /tmux/key ", 15) == 0)
    {
        const char *body = conn->input + conn->header_bytes;
        size_t body_len = request_len - conn->header_bytes;
        char raw_sess[64] = {0}, key[32] = {0};
        extract_json_str_field(body, body_len, "session", raw_sess, sizeof(raw_sess));
        extract_json_str_field(body, body_len, "key", key, sizeof(key));

        char sess[64];
        sanitize_tmux_name(raw_sess, sess, sizeof(sess));
        const char *tmux_bin = find_tmux_bin();

        if (strcmp(key, "kill-session") == 0)
        {
            const char *kill_argv[] = {tmux_bin, "kill-session", "-t", sess, NULL};
            int rc = run_exec_capture(kill_argv, NULL, 0);
            char jbuf[256];
            snprintf(jbuf, sizeof(jbuf), "{\"ok\":%s,\"session\":\"%s\",\"key\":\"kill-session\"}", rc == 0 ? "true" : "false", sess);
            send_json_response(conn, 200, "OK", jbuf);
        }
        else if (!is_allowed_tmux_key(key))
        {
            send_json_response(conn, 400, "Bad Request", "{\"error\":\"disallowed tmux key\"}");
        }
        else
        {
            const char *k_argv[] = {tmux_bin, "send-keys", "-t", sess, key, NULL};
            int rc = run_exec_capture(k_argv, NULL, 0);
            char jbuf[256];
            snprintf(jbuf, sizeof(jbuf), "{\"ok\":%s,\"session\":\"%s\",\"key\":\"%s\"}", rc == 0 ? "true" : "false", sess, key);
            send_json_response(conn, 200, "OK", jbuf);
        }
    }
    else if (strncmp(conn->input, "POST /api/run ", 14) == 0)
    {
        const char *body = conn->input + conn->header_bytes;
        size_t body_len = request_len - conn->header_bytes;
        char mode[32] = "command";
        char workspace[64] = "";
        char command[1024] = "";
        char prompt[3072] = "";
        char owned[512] = "";
        char model[128] = "qwen2.5-coder:7b";
        char label[64] = "";

        extract_json_str_field(body, body_len, "mode", mode, sizeof(mode));
        if (mode[0] == '\0') strcpy(mode, "command");
        extract_json_str_field(body, body_len, "workspace", workspace, sizeof(workspace));
        extract_json_str_field(body, body_len, "command", command, sizeof(command));
        extract_json_str_field(body, body_len, "prompt", prompt, sizeof(prompt));
        extract_json_str_field(body, body_len, "owned", owned, sizeof(owned));
        extract_json_str_field(body, body_len, "model", model, sizeof(model));
        if (model[0] == '\0') strcpy(model, "qwen2.5-coder:7b");
        extract_json_str_field(body, body_len, "label", label, sizeof(label));

        char safe_ws[64] = "";
        size_t wsi = 0;
        for (size_t k = 0; workspace[k] && wsi < sizeof(safe_ws) - 1; k++)
        {
            char c = workspace[k];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')
                safe_ws[wsi++] = c;
        }
        safe_ws[wsi] = '\0';

        char manifest_json[4096];
        uint16_t env_flags = JULY_FLAG_NONE;
        const char *attach_name = NULL;
        const char *attach_data = NULL;
        size_t attach_len = 0;

        if (strcmp(mode, "julyos_boot") == 0)
        {
            env_flags = JULY_FLAG_WORKSPACE;
            snprintf(manifest_json, sizeof(manifest_json),
                "{\"name\":\"julyos\",\"options\":{\"timeout_sec\":90,\"kill_if_timeout\":true},"
                "\"sandbox\":{\"network\":false,\"namespace\":false},"
                "\"build\":[\"make clean\",\"make qemu-demo\"],"
                "\"artifacts\":{\"required\":[\"julyos.elf\",\"julyos_screen.png\",\"serial_boot.log\"]}}");
            if (label[0] == '\0') strcpy(label, "julyos:boot");
        }
        else if (strcmp(mode, "agent") == 0 && prompt[0] != '\0')
        {
            if (safe_ws[0] == '\0') strcpy(safe_ws, "july");
            char esc_owned[640], esc_model[192];
            json_escape_append(esc_owned, 0, sizeof(esc_owned), owned);
            json_escape_append(esc_model, 0, sizeof(esc_model), model);
            snprintf(manifest_json, sizeof(manifest_json),
                "{\"name\":\"pocket-agent\",\"options\":{\"timeout_sec\":600,\"kill_if_timeout\":true},"
                "\"sandbox\":{\"network\":true,\"namespace\":false},"
                "\"env\":{\"JULY_BACKEND\":\"ollama\",\"OLLAMA_MODEL\":\"%s\",\"JULY_OWNED_FILES\":\"%s\"},"
                "\"build\":[\"bash -c 'cp -r /tmp/july_workspaces/%s/. . 2>/dev/null || true; chmod +x harness/july_harness.sh && ./harness/july_harness.sh agent_brief.md'\"],"
                "\"artifacts\":{\"required\":[\"agent_patch.diff\",\"agent_files.txt\",\"agent_run.log\"],\"on_failure\":[\"agent_run.log\"]}}",
                esc_model, esc_owned, safe_ws);
            attach_name = "agent_brief.md";
            attach_data = prompt;
            attach_len = strlen(prompt);
            if (label[0] == '\0') snprintf(label, sizeof(label), "agent:%s", safe_ws);
        }
        else if (command[0] != '\0')
        {
            char esc_cmd[1536];
            json_escape_append(esc_cmd, 0, sizeof(esc_cmd), command);
            if (safe_ws[0] != '\0') env_flags = JULY_FLAG_WORKSPACE;
            else strcpy(safe_ws, "pocket-cmd");
            snprintf(manifest_json, sizeof(manifest_json),
                "{\"name\":\"%s\",\"options\":{\"timeout_sec\":120,\"kill_if_timeout\":true},"
                "\"sandbox\":{\"network\":true,\"namespace\":false},"
                "\"build\":[\"%s\"]}",
                safe_ws, esc_cmd);
            if (label[0] == '\0') snprintf(label, sizeof(label), "%.48s", command);
        }
        else
        {
            send_json_response(conn, 400, "Bad Request", "{\"error\":\"Missing command or prompt\"}");
            manifest_json[0] = '\0';
        }

        if (manifest_json[0] != '\0')
        {
            size_t mlen = strlen(manifest_json);
            uint32_t nf = attach_name ? 1U : 0U;
            size_t fnl = attach_name ? strlen(attach_name) : 0;
            size_t env_sz = 12 + mlen + 4 + (attach_name ? (2 + fnl + 2 + 8 + attach_len) : 0);
            uint8_t *env_buf = malloc(env_sz);
            if (!env_buf)
            {
                send_json_response(conn, 500, "Internal Server Error", "{\"error\":\"oom\"}");
            }
            else
            {
                env_buf[0] = 'J'; env_buf[1] = 'U'; env_buf[2] = 'L'; env_buf[3] = 'Y';
                write_uint16_le(env_buf + 4, JULY_ENVELOPE_VERSION);
                write_uint16_le(env_buf + 6, env_flags);
                write_uint32_le(env_buf + 8, (uint32_t)mlen);
                memcpy(env_buf + 12, manifest_json, mlen);
                write_uint32_le(env_buf + 12 + mlen, nf);
                if (attach_name)
                {
                    uint8_t *p = env_buf + 12 + mlen + 4;
                    write_uint16_le(p, (uint16_t)fnl); p += 2;
                    memcpy(p, attach_name, fnl); p += fnl;
                    write_uint16_le(p, 0644); p += 2;
                    write_uint64_le(p, (uint64_t)attach_len); p += 8;
                    if (attach_len > 0) memcpy(p, attach_data, attach_len);
                }

                Task task = {0};
                task.type = TASK_TYPE_WORKFLOW;
                snprintf(task.taskId, sizeof(task.taskId), "wf_%ld_%d", (long)time(NULL), rand() % 100000);
                snprintf(task.storageDir, sizeof(task.storageDir), "/tmp/july_storage/%s", task.taskId);
                snprintf(task.sandboxDir, sizeof(task.sandboxDir), "/tmp/july_runs/%s", task.taskId);
                mkdir_p(task.storageDir);

                char envelope_path[512];
                snprintf(envelope_path, sizeof(envelope_path), "%s/envelope.bin", task.storageDir);
                int ef = open(envelope_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (ef >= 0) { write(ef, env_buf, env_sz); close(ef); }
                free(env_buf);

                if (enqueue_task(&task) == 0)
                {
                    task_result_record_enqueued(task.taskId);
                    task_result_set_meta(task.taskId, label, true);
                    char jbuf[256];
                    snprintf(jbuf, sizeof(jbuf), "{\"task_id\":\"%s\",\"status\":\"queued\"}", task.taskId);
                    send_json_response(conn, 202, "Accepted", jbuf);
                }
                else
                {
                    send_json_response(conn, 503, "Service Unavailable", "{\"error\":\"queue full\"}");
                }
            }
        }
    }
    else if (conn->input[0] == 'P' && strncmp(conn->input, "POST /workflow ", 15) == 0)
    {
        // Binary multi-file envelope workflow submission
        const char *body = conn->input + conn->header_bytes;
        size_t declared_len = conn->content_len;
        size_t received_body_len = request_len - conn->header_bytes;

        if (unlikely(received_body_len < declared_len || declared_len < 16))
        {
            write_response(conn, response_400, response_400_len);
        }
        else
        {
            uint32_t magic = read_uint32_le((const uint8_t *)body);
            if (memcmp(body, "JULY", 4) != 0 && magic != JULY_ENVELOPE_MAGIC && magic != 0x594C554A)
            {
                write_response(conn, response_400, response_400_len);
            }
            else
            {
                Task task = {0};
                task.type = TASK_TYPE_WORKFLOW;
                snprintf(task.taskId, sizeof(task.taskId), "wf_%ld_%d", (long)time(NULL), rand() % 100000);
                snprintf(task.storageDir, sizeof(task.storageDir), "/tmp/july_storage/%s", task.taskId);
                snprintf(task.sandboxDir, sizeof(task.sandboxDir), "/tmp/july_runs/%s", task.taskId);

                mkdir_p(task.storageDir);

                // Write envelope to WAL file
                char envelope_path[512];
                snprintf(envelope_path, sizeof(envelope_path), "%s/envelope.bin", task.storageDir);
                int ef = open(envelope_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (ef >= 0)
                {
                    write(ef, body, declared_len);
                    close(ef);
                }

                if (likely(enqueue_task(&task) == 0))
                {
                    task_result_record_enqueued(task.taskId);
                    char resp[256];
                    int rlen = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 202 Accepted\r\n"
                        "Content-Type: text/plain\r\n"
                        "X-Task-Id: %s\r\n"
                        "Content-Length: 15\r\n"
                        "\r\n"
                        "Workflow Queued",
                        task.taskId);
                    write_response(conn, resp, (size_t)rlen);
                }
                else
                {
                    write_response(conn, response_503, response_503_len);
                }
            }
        }
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
            {
                if (likely(task.taskCommand[0] == ':' && task.taskCommand[1] == '\0'))
                {
                    task_result_record_completed(task.taskId, 0, "", 0);
                }
                else
                {
                    task_result_record_enqueued(task.taskId);
                }
                write_response(conn, response_202, response_202_len);
            }
            else
            {
                write_response(conn, response_503, response_503_len);
            }
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
#if defined(MSG_NOSIGNAL)
        ssize_t n = send(conn->fd,
                         conn->output + conn->output_offset,
                         conn->output_len - conn->output_offset,
                         MSG_NOSIGNAL);
#else
        ssize_t n = send(conn->fd,
                         conn->output + conn->output_offset,
                         conn->output_len - conn->output_offset,
                         0);
#endif
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

static void print_server_network_banner(int port, const char *backend)
{
    printf("\n┌─────────────────────────────────────────────────────────────┐\n");
    printf("│  July Task & Workflow Engine (%-6s backend)            │\n", backend);
    printf("│  Listening on: 0.0.0.0:%-5d (All LAN interfaces)          │\n", port);
    printf("│  Worker Pool:  %d threads                                   │\n", NUM_WORKERS);
    if (g_auth_token[0] != '\0')
    {
        printf("│  Auth Mode:    Protected (Token Enforced)                   │\n");
    }
    else
    {
        printf("│  Auth Mode:    Open LAN (No token configured)               │\n");
    }
    printf("├─────────────────────────────────────────────────────────────┤\n");
    printf("│  Connect with dispatcher from your Mac / LAN:               │\n");

    struct ifaddrs *ifaddr, *ifa;
    int found_lan = 0;
    if (getifaddrs(&ifaddr) == 0)
    {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
                continue;

            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), ip_str, sizeof(ip_str));

            if (strcmp(ip_str, "127.0.0.1") == 0)
            {
                printf("│    • Local loopback:  http://%s:%-5d                  │\n", ip_str, port);
            }
            else
            {
                printf("│    • LAN (%-8s): http://%-15s:%-5d          │\n", ifa->ifa_name, ip_str, port);
                found_lan = 1;
            }
        }
        freeifaddrs(ifaddr);
    }
    if (!found_lan)
    {
        printf("│    • No external LAN IP detected                            │\n");
    }
    printf("└─────────────────────────────────────────────────────────────┘\n\n");
}

int main(int argc, char **argv)
{
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    srand((unsigned int)time(NULL));

    int port = PORT;
    const char *env_port = getenv("PORT");
    if (env_port)
    {
        int p = atoi(env_port);
        if (p > 0 && p <= 65535)
            port = p;
    }

    const char *env_tok = getenv("JULY_TOKEN");
    if (env_tok) safe_copy_str(g_auth_token, sizeof(g_auth_token), env_tok);

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "--token") == 0 || strcmp(argv[i], "-k") == 0) && i + 1 < argc)
        {
            safe_copy_str(g_auth_token, sizeof(g_auth_token), argv[++i]);
        }
        else if (argv[i][0] != '-')
        {
            int p = atoi(argv[i]);
            if (p > 0 && p <= 65535)
                port = p;
        }
    }

    if (g_auth_token[0] == '\0')
    {
        char token_file[512];
        const char *home = getenv("HOME");
        if (home)
        {
            snprintf(token_file, sizeof(token_file), "%s/.july/token", home);
            FILE *tf = fopen(token_file, "r");
            if (tf)
            {
                if (fgets(g_auth_token, sizeof(g_auth_token), tf))
                {
                    size_t tlen = strlen(g_auth_token);
                    while (tlen > 0 && (g_auth_token[tlen - 1] == '\r' || g_auth_token[tlen - 1] == '\n' || g_auth_token[tlen - 1] == ' '))
                        g_auth_token[--tlen] = '\0';
                }
                fclose(tf);
            }
        }
    }

    pthread_mutex_init(&queue.lock, NULL);
    pthread_cond_init(&queue.not_empty, NULL);
    init_result_store();

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
    address.sin_port = htons(port);
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

#if defined(__APPLE__)
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

    print_server_network_banner(port, "kqueue");

    struct kevent events[256];
    while (g_running)
    {
        int ready = kevent(kq, NULL, 0, events, 256, NULL);
        if (unlikely(ready < 0))
        {
            if (errno == EINTR)
            {
                if (!g_running)
                    break;
                continue;
            }
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
                    conn->input_cap = INITIAL_BUFFER_SIZE;
                    conn->input = malloc(conn->input_cap);
                    if (!conn->input)
                    {
                        close(client_fd);
                        free(conn);
                        continue;
                    }

                    struct kevent client_events[2];
                    EV_SET(&client_events[0], client_fd, EVFILT_READ,
                           EV_ADD | EV_ENABLE, 0, 0, conn);
                    EV_SET(&client_events[1], client_fd, EVFILT_WRITE,
                           EV_ADD | EV_DISABLE, 0, 0, conn);
                    if (unlikely(kevent(kq, client_events, 2, NULL, 0, NULL) < 0))
                    {
                        close_connection(kq, conn);
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
                    if (conn->input_len >= conn->input_cap - 1)
                    {
                        size_t new_cap = conn->input_cap * 2;
                        if (new_cap > MAX_PAYLOAD_SIZE)
                        {
                            failed = true;
                            break;
                        }
                        char *nb = realloc(conn->input, new_cap);
                        if (!nb)
                        {
                            failed = true;
                            break;
                        }
                        conn->input = nb;
                        conn->input_cap = new_cap;
                    }
                    ssize_t n = recv(conn->fd,
                                     conn->input + conn->input_len,
                                     conn->input_cap - 1 - conn->input_len, 0);
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
                        int p_res = prepare_response(conn, (size_t)request_len);
                        if (p_res == 1)
                        {
                            close_connection(kq, conn);
                            continue;
                        }
                        int wr = flush_response(conn);
                        if (wr < 0)
                            failed = true;
                        else if (wr == 0 && conn->close_after_write)
                        {
                            close_connection(kq, conn);
                            continue;
                        }
                        else if (wr == 1)
                        {
                            if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                                failed = true;
                        }
                        else
                        {
                            ssize_t next_len = complete_request_size(conn);
                            if (next_len > 0)
                            {
                                int p_res2 = prepare_response(conn, (size_t)next_len);
                                if (p_res2 == 1)
                                {
                                    close_connection(kq, conn);
                                    continue;
                                }
                                int wr2 = flush_response(conn);
                                if (wr2 < 0)
                                    failed = true;
                                else if (wr2 == 0 && conn->close_after_write)
                                {
                                    close_connection(kq, conn);
                                    continue;
                                }
                                else if (wr2 == 1)
                                {
                                    if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                                        failed = true;
                                }
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
                        int p_res = prepare_response(conn, (size_t)next_len);
                        if (p_res == 1)
                        {
                            close_connection(kq, conn);
                            continue;
                        }
                        int wr2 = flush_response(conn);
                        if (wr2 < 0)
                            failed = true;
                        else if (wr2 == 0 && conn->close_after_write)
                        {
                            close_connection(kq, conn);
                            continue;
                        }
                        else if (wr2 == 1)
                        {
                            if (update_interest(kq, conn, EVFILT_WRITE) < 0)
                                failed = true;
                        }
                        else if (update_interest(kq, conn, EVFILT_READ) < 0)
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

#else // Linux / epoll

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
    {
        perror("epoll_create1 failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event listen_event = {
        .events = EPOLLIN,
        .data.ptr = NULL
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &listen_event) < 0)
    {
        perror("epoll_ctl listener failed");
        close(epfd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    print_server_network_banner(port, "epoll");

    struct epoll_event events[256];
    while (g_running)
    {
        int ready = epoll_wait(epfd, events, 256, -1);
        if (unlikely(ready < 0))
        {
            if (errno == EINTR)
            {
                if (!g_running)
                    break;
                continue;
            }
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < ready; i++)
        {
            if (events[i].data.ptr == NULL)
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
                    conn->input_cap = INITIAL_BUFFER_SIZE;
                    conn->input = malloc(conn->input_cap);
                    if (!conn->input)
                    {
                        close(client_fd);
                        free(conn);
                        continue;
                    }

                    struct epoll_event client_ev = {
                        .events = EPOLLIN | EPOLLRDHUP,
                        .data.ptr = conn
                    };
                    if (unlikely(epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0))
                    {
                        close_connection(epfd, conn);
                    }
                }
                continue;
            }

            Connection *conn = (Connection *)events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (unlikely(ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)))
            {
                close_connection(epfd, conn);
                continue;
            }

            bool failed = false;
            if (ev & EPOLLIN)
            {
                for (;;)
                {
                    if (conn->input_len >= conn->input_cap - 1)
                    {
                        size_t new_cap = conn->input_cap * 2;
                        if (new_cap > MAX_PAYLOAD_SIZE)
                        {
                            failed = true;
                            break;
                        }
                        char *nb = realloc(conn->input, new_cap);
                        if (!nb)
                        {
                            failed = true;
                            break;
                        }
                        conn->input = nb;
                        conn->input_cap = new_cap;
                    }
                    ssize_t n = recv(conn->fd,
                                     conn->input + conn->input_len,
                                     conn->input_cap - 1 - conn->input_len, 0);
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
                        int p_res = prepare_response(conn, (size_t)request_len);
                        if (p_res == 1)
                        {
                            close_connection(epfd, conn);
                            continue;
                        }
                        int wr = flush_response(conn);
                        if (wr < 0)
                            failed = true;
                        else if (wr == 0 && conn->close_after_write)
                        {
                            close_connection(epfd, conn);
                            continue;
                        }
                        else if (wr == 1)
                        {
                            if (update_interest(epfd, conn, EPOLLIN | EPOLLOUT) < 0)
                                failed = true;
                        }
                        else
                        {
                            ssize_t next_len = complete_request_size(conn);
                            if (next_len > 0)
                            {
                                int p_res2 = prepare_response(conn, (size_t)next_len);
                                if (p_res2 == 1)
                                {
                                    close_connection(epfd, conn);
                                    continue;
                                }
                                int wr2 = flush_response(conn);
                                if (wr2 < 0)
                                    failed = true;
                                else if (wr2 == 0 && conn->close_after_write)
                                {
                                    close_connection(epfd, conn);
                                    continue;
                                }
                                else if (wr2 == 1)
                                {
                                    if (update_interest(epfd, conn, EPOLLIN | EPOLLOUT) < 0)
                                        failed = true;
                                }
                            }
                            else if (next_len < 0)
                                failed = true;
                        }
                    }
                }
            }

            if (!failed && (ev & EPOLLOUT))
            {
                int wr = flush_response(conn);
                if (wr < 0)
                    failed = true;
                else if (wr == 0)
                {
                    if (conn->close_after_write)
                    {
                        close_connection(epfd, conn);
                        continue;
                    }
                    ssize_t next_len = complete_request_size(conn);
                    if (next_len > 0)
                    {
                        int p_res = prepare_response(conn, (size_t)next_len);
                        if (p_res == 1)
                        {
                            close_connection(epfd, conn);
                            continue;
                        }
                        int wr2 = flush_response(conn);
                        if (wr2 < 0)
                            failed = true;
                        else if (wr2 == 0 && conn->close_after_write)
                        {
                            close_connection(epfd, conn);
                            continue;
                        }
                        else if (wr2 == 1)
                        {
                            if (update_interest(epfd, conn, EPOLLIN | EPOLLOUT) < 0)
                                failed = true;
                        }
                        else
                        {
                            if (update_interest(epfd, conn, EPOLLIN) < 0)
                                failed = true;
                        }
                    }
                    else if (next_len < 0)
                        failed = true;
                    else
                    {
                        if (update_interest(epfd, conn, EPOLLIN) < 0)
                            failed = true;
                    }
                }
            }

            if (failed)
                close_connection(epfd, conn);
        }
    }

    close(epfd);
#endif

    close(server_fd);
    return 0;
}
