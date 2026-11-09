#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "task.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_TIMEOUT_SEC 15
#define RESPONSE_BUF_SIZE 65536

/* ANSI Color Escape Codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_MAGENTA "\033[1;35m"

static bool use_colors = true;

static inline void pack_uint32_le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFF);
    dst[1] = (uint8_t)((val >> 8) & 0xFF);
    dst[2] = (uint8_t)((val >> 16) & 0xFF);
    dst[3] = (uint8_t)((val >> 24) & 0xFF);
}

static bool parse_target_spec(const char *arg, char *out_host, size_t max_host, int *out_port)
{
    char temp[256];
    strncpy(temp, arg, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *colon = strchr(temp, ':');
    if (colon)
    {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535)
        {
            *out_port = p;
            strncpy(out_host, temp, max_host - 1);
            out_host[max_host - 1] = '\0';
            return true;
        }
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, temp, &addr) == 1)
    {
        strncpy(out_host, temp, max_host - 1);
        out_host[max_host - 1] = '\0';
        return true;
    }

    // Check if it ends in common local domain (e.g. .local or localhost)
    if (strcmp(temp, "localhost") == 0 || strstr(temp, ".local"))
    {
        strncpy(out_host, temp, max_host - 1);
        out_host[max_host - 1] = '\0';
        return true;
    }

    return false;
}

static int connect_to_server(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0)
    {
        return -1;
    }

    int sock = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next)
    {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0)
            continue;

        // Set socket timeouts to prevent client hanging if remote host is unresponsive
        struct timeval tv = {.tv_sec = DEFAULT_TIMEOUT_SEC, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break; // Success

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    return sock;
}

/* Dispatch task binary payload to POST /task */
static int dispatch_task(const char *host, int port, const char *task_id, const char *command)
{
    size_t id_len = strlen(task_id);
    size_t cmd_len = strlen(command);

    if (id_len >= MAX_TASK_ID)
    {
        fprintf(stderr, "%s[!] Task ID too long (max %d bytes)%s\n",
                use_colors ? COLOR_RED : "", MAX_TASK_ID - 1, use_colors ? COLOR_RESET : "");
        return -1;
    }
    if (cmd_len >= MAX_TASK_CMD)
    {
        fprintf(stderr, "%s[!] Command too long (max %d bytes)%s\n",
                use_colors ? COLOR_RED : "", MAX_TASK_CMD - 1, use_colors ? COLOR_RESET : "");
        return -1;
    }

    size_t payload_len = sizeof(TaskHeader) + id_len + cmd_len;
    uint8_t *payload = malloc(payload_len);
    if (!payload)
    {
        perror("malloc");
        return -1;
    }

    pack_uint32_le(payload, (uint32_t)id_len);
    pack_uint32_le(payload + 4, (uint32_t)cmd_len);
    memcpy(payload + sizeof(TaskHeader), task_id, id_len);
    memcpy(payload + sizeof(TaskHeader) + id_len, command, cmd_len);

    char headers[512];
    int header_len = snprintf(headers, sizeof(headers),
        "POST /task HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, payload_len);

    int sock = connect_to_server(host, port);
    if (sock < 0)
    {
        fprintf(stderr, "%s[!] Failed to connect to server at %s:%d%s\n",
                use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
        fprintf(stderr, "%s    -> Ensure the server is running on the target machine and port %d is open in the firewall (e.g. 'sudo ufw allow %d/tcp').%s\n",
                use_colors ? COLOR_YELLOW : "", port, port, use_colors ? COLOR_RESET : "");
        free(payload);
        return -1;
    }

    if (write(sock, headers, header_len) != header_len)
    {
        perror("write headers");
        close(sock);
        free(payload);
        return -1;
    }

    if ((size_t)write(sock, payload, payload_len) != payload_len)
    {
        perror("write payload");
        close(sock);
        free(payload);
        return -1;
    }
    free(payload);

    char response[1024];
    ssize_t n = read(sock, response, sizeof(response) - 1);
    close(sock);

    if (n <= 0)
    {
        fprintf(stderr, "%s[!] Empty or failed response from server%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        return -1;
    }
    response[n] = '\0';

    int status_code = 0;
    if (sscanf(response, "HTTP/1.1 %d", &status_code) != 1)
    {
        fprintf(stderr, "%s[!] Invalid HTTP response:%s\n%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "", response);
        return -1;
    }

    if (status_code != 202)
    {
        fprintf(stderr, "%s[!] Task submission rejected (HTTP %d)%s\n",
                use_colors ? COLOR_RED : "", status_code, use_colors ? COLOR_RESET : "");
        return -1;
    }

    return 0; // Successfully queued
}

/* Retrieve task status and output from GET /retrieve/<taskId> */
static int retrieve_task(const char *host, int port, const char *task_id,
                         int *exit_code, char **output_body, size_t *output_len)
{
    int sock = connect_to_server(host, port);
    if (sock < 0)
        return -1;

    char request[256];
    int req_len = snprintf(request, sizeof(request),
        "GET /retrieve/%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        task_id, host, port);

    if (write(sock, request, req_len) != req_len)
    {
        close(sock);
        return -1;
    }

    size_t capacity = RESPONSE_BUF_SIZE;
    size_t total_read = 0;
    char *buf = malloc(capacity);
    if (!buf)
    {
        close(sock);
        return -1;
    }

    while (1)
    {
        if (total_read >= capacity - 1)
        {
            capacity *= 2;
            char *new_buf = realloc(buf, capacity);
            if (!new_buf)
            {
                free(buf);
                close(sock);
                return -1;
            }
            buf = new_buf;
        }

        ssize_t n = read(sock, buf + total_read, capacity - 1 - total_read);
        if (n <= 0)
            break;
        total_read += (size_t)n;
    }
    close(sock);

    buf[total_read] = '\0';

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.1 %d", &status_code) != 1)
    {
        free(buf);
        return -1;
    }

    if (status_code == 202)
    {
        free(buf);
        return 1; // Still running
    }

    if (status_code == 404)
    {
        free(buf);
        return -2; // Not found
    }

    if (status_code != 200)
    {
        free(buf);
        return -1;
    }

    *exit_code = 0;
    char *code_hdr = strcasestr(buf, "X-Task-Exit-Code:");
    if (code_hdr)
    {
        sscanf(code_hdr + 17, "%d", exit_code);
    }

    char *body_start = strstr(buf, "\r\n\r\n");
    if (!body_start)
    {
        free(buf);
        return -1;
    }
    body_start += 4;

    size_t b_len = (size_t)(buf + total_read - body_start);
    char *out = malloc(b_len + 1);
    if (out)
    {
        memcpy(out, body_start, b_len);
        out[b_len] = '\0';
        *output_body = out;
        *output_len = b_len;
    }

    free(buf);
    return 0; // Completed
}

static int wait_for_task_completion(const char *host, int port, const char *task_id,
                                    int timeout_sec, int *exit_code,
                                    char **output, size_t *output_len)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1)
    {
        int res = retrieve_task(host, port, task_id, exit_code, output, output_len);
        if (res == 0)
        {
            return 0; // Finished
        }
        if (res == -2)
        {
            usleep(10000); // 10 ms
        }
        else if (res < 0)
        {
            return -1; // Error
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_sec)
        {
            return -3; // Timed out waiting
        }

        usleep(25000); // 25 ms polling interval
    }
}

static void print_help(const char *prog)
{
    printf("\n%sTask Dispatcher (LAN & Local)%s\n\n",
           use_colors ? COLOR_BOLD : "", use_colors ? COLOR_RESET : "");
    printf("Usage:\n");
    printf("  %s [host:port] [command]           Execute command on local or remote machine\n", prog);
    printf("  %s [host:port]                     Open interactive shell targeting host\n", prog);
    printf("  %s                                 Open interactive shell (default: 127.0.0.1:8080)\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <host>       Server IP or hostname (default: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <port>       Server port (default: %d)\n", DEFAULT_PORT);
    printf("  -t, --timeout <sec>     Execution timeout in seconds (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  -i, --id <taskId>       Custom task ID (default: auto-generated)\n");
    printf("  -a, --async             Dispatch task asynchronously without waiting for output\n");
    printf("  -g, --get <taskId>      Retrieve output of a previously dispatched task\n");
    printf("  --no-color              Disable ANSI colors\n");
    printf("  --help                  Show this help message\n\n");
    printf("LAN Examples (Mac -> Linux):\n");
    printf("  %s 192.168.1.50 \"uname -a\"\n", prog);
    printf("  %s 192.168.1.50:9000 \"df -h\"\n", prog);
    printf("  %s 192.168.1.50                    (Starts interactive shell to Linux machine)\n", prog);
    printf("  %s -h 192.168.1.50 -p 8080 \"free -m\"\n\n", prog);
}

static double ping_server(const char *host, int port)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int s = connect_to_server(host, port);
    if (s < 0)
        return -1.0;

    const char *ping_req = "GET / HTTP/1.1\r\nHost: ping\r\nConnection: close\r\n\r\n";
    if (write(s, ping_req, strlen(ping_req)) <= 0)
    {
        close(s);
        return -1.0;
    }

    char buf[128];
    ssize_t n = read(s, buf, sizeof(buf));
    close(s);

    if (n <= 0)
        return -1.0;

    clock_gettime(CLOCK_MONOTONIC, &end);
    return ((double)(end.tv_sec - start.tv_sec) * 1000.0) +
           ((double)(end.tv_nsec - start.tv_nsec) / 1e6);
}

int main(int argc, char **argv)
{
    use_colors = isatty(fileno(stdout));

    char host[256];
    strncpy(host, DEFAULT_HOST, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    int port = DEFAULT_PORT;
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    const char *custom_task_id = NULL;
    const char *retrieve_id = NULL;
    bool async_mode = false;
    char command_buf[MAX_TASK_CMD] = {0};

    // Environment overrides
    const char *env_host = getenv("DISPATCHER_HOST");
    if (env_host) strncpy(host, env_host, sizeof(host) - 1);
    const char *env_port = getenv("DISPATCHER_PORT");
    if (env_port) port = atoi(env_port);

    bool host_set_by_pos = false;

    // Argument parsing
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            print_help(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--no-color") == 0)
        {
            use_colors = false;
        }
        else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--async") == 0)
        {
            async_mode = true;
        }
        else if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc)
        {
            strncpy(host, argv[++i], sizeof(host) - 1);
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
        {
            port = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) && i + 1 < argc)
        {
            timeout_sec = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--id") == 0) && i + 1 < argc)
        {
            custom_task_id = argv[++i];
        }
        else if ((strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--get") == 0) && i + 1 < argc)
        {
            retrieve_id = argv[++i];
        }
        else if (argv[i][0] != '-')
        {
            // If the first positional argument is an IP or host:port, treat it as target
            if (!host_set_by_pos && command_buf[0] == '\0')
            {
                char parsed_host[256];
                int parsed_port = port;
                if (parse_target_spec(argv[i], parsed_host, sizeof(parsed_host), &parsed_port))
                {
                    strncpy(host, parsed_host, sizeof(host) - 1);
                    port = parsed_port;
                    host_set_by_pos = true;
                    continue;
                }
            }

            // Append positional argument to command buffer
            if (command_buf[0] != '\0')
            {
                strncat(command_buf, " ", sizeof(command_buf) - strlen(command_buf) - 1);
            }
            strncat(command_buf, argv[i], sizeof(command_buf) - strlen(command_buf) - 1);
        }
    }

    // If query mode requested:
    if (retrieve_id)
    {
        printf("%s[*] Querying task '%s' from %s:%d...%s\n",
               use_colors ? COLOR_BLUE : "", retrieve_id, host, port, use_colors ? COLOR_RESET : "");
        int exit_code = 0;
        char *output = NULL;
        size_t out_len = 0;
        int res = retrieve_task(host, port, retrieve_id, &exit_code, &output, &out_len);
        if (res == 0)
        {
            printf("%s[✓] Task '%s' Completed (Exit Code: %d):%s\n",
                   use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
                   retrieve_id, exit_code, use_colors ? COLOR_RESET : "");
            if (out_len > 0 && output)
            {
                fwrite(output, 1, out_len, stdout);
                if (output[out_len - 1] != '\n')
                    putchar('\n');
            }
            free(output);
            return exit_code;
        }
        else if (res == 1)
        {
            printf("%s[⏳] Task '%s' is still queued or executing.%s\n",
                   use_colors ? COLOR_YELLOW : "", retrieve_id, use_colors ? COLOR_RESET : "");
            return 0;
        }
        else if (res == -2)
        {
            fprintf(stderr, "%s[!] Task '%s' not found on server.%s\n",
                    use_colors ? COLOR_RED : "", retrieve_id, use_colors ? COLOR_RESET : "");
            return 1;
        }
        else
        {
            fprintf(stderr, "%s[!] Failed to retrieve task '%s'.%s\n",
                    use_colors ? COLOR_RED : "", retrieve_id, use_colors ? COLOR_RESET : "");
            return 1;
        }
    }

    // One-shot command execution
    if (command_buf[0] != '\0')
    {
        char task_id[MAX_TASK_ID];
        if (custom_task_id)
        {
            snprintf(task_id, sizeof(task_id), "%s", custom_task_id);
        }
        else
        {
            snprintf(task_id, sizeof(task_id), "task_%ld_%d", (long)time(NULL), getpid());
        }

        printf("%s[+] Target: %s:%d | Task: %s%s\n",
               use_colors ? COLOR_CYAN : "", host, port, task_id, use_colors ? COLOR_RESET : "");
        printf("%s[>] Command:%s %s%s%s\n",
               use_colors ? COLOR_BLUE : "", use_colors ? COLOR_RESET : "",
               use_colors ? COLOR_BOLD : "", command_buf, use_colors ? COLOR_RESET : "");

        if (dispatch_task(host, port, task_id, command_buf) != 0)
            return 1;

        if (async_mode)
        {
            printf("%s[✓] Task queued in async mode.%s Retrieve anytime with: %s -h %s -p %d -g %s\n",
                   use_colors ? COLOR_GREEN : "", use_colors ? COLOR_RESET : "",
                   argv[0], host, port, task_id);
            return 0;
        }

        int exit_code = 0;
        char *output = NULL;
        size_t out_len = 0;
        int res = wait_for_task_completion(host, port, task_id, timeout_sec,
                                           &exit_code, &output, &out_len);
        if (res == 0)
        {
            printf("%s[✓] Completed (exit=%d):%s\n",
                   use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
                   exit_code, use_colors ? COLOR_RESET : "");
            if (out_len > 0 && output)
            {
                fwrite(output, 1, out_len, stdout);
                if (output[out_len - 1] != '\n')
                    putchar('\n');
            }
            free(output);
            return exit_code;
        }
        else if (res == -3)
        {
            fprintf(stderr, "%s[!] Task execution timed out after %ds (task still running on server)%s\n",
                    use_colors ? COLOR_RED : "", timeout_sec, use_colors ? COLOR_RESET : "");
            return 124;
        }
        else
        {
            fprintf(stderr, "%s[!] Failed to get task results from %s:%d%s\n",
                    use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
            return 1;
        }
    }

    // ─── Interactive Shell REPL Mode ───
    printf("%s┌─────────────────────────────────────────────────────────────┐%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│             Task Dispatcher (LAN & Local CLI)               │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│  Target: %-49s  │%s\n",
           use_colors ? COLOR_CYAN : "", host, use_colors ? COLOR_RESET : "");
    printf("%s│  Port:   %-49d  │%s\n",
           use_colors ? COLOR_CYAN : "", port, use_colors ? COLOR_RESET : "");
    printf("%s│  Commands: 'ping', 'target <ip>', 'help', 'exit'            │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s└─────────────────────────────────────────────────────────────┘%s\n\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");

    // Quick connectivity check
    double rtt = ping_server(host, port);
    if (rtt >= 0.0)
    {
        printf("%s[✓] Connected to %s:%d (RTT: %.2f ms)%s\n\n",
               use_colors ? COLOR_GREEN : "", host, port, rtt, use_colors ? COLOR_RESET : "");
    }
    else
    {
        printf("%s[!] Cannot connect to %s:%d. Make sure the server is running and firewall allows port %d.%s\n\n",
               use_colors ? COLOR_RED : "", host, port, port, use_colors ? COLOR_RESET : "");
    }

    char line[MAX_TASK_CMD];
    int task_counter = 1;

    while (1)
    {
        printf("%sdispatcher%s(%s%s:%d%s)> ",
               use_colors ? COLOR_GREEN : "", use_colors ? COLOR_RESET : "",
               use_colors ? COLOR_CYAN : "", host, port, use_colors ? COLOR_RESET : "");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
        {
            printf("\nExiting...\n");
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
        {
            line[--len] = '\0';
        }

        if (len == 0)
            continue;

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
        {
            printf("Goodbye.\n");
            break;
        }
        if (strcmp(line, "help") == 0)
        {
            printf("Built-in Dispatcher commands:\n");
            printf("  ping / status         Check connection and round-trip latency to %s:%d\n", host, port);
            printf("  target <ip> [port]    Switch target server (e.g. 'target 192.168.1.50')\n");
            printf("  exit / quit           Exit interactive mode\n");
            printf("All other commands are dispatched to the remote Linux/POSIX worker.\n");
            continue;
        }
        if (strcmp(line, "ping") == 0 || strcmp(line, "status") == 0)
        {
            double ping_ms = ping_server(host, port);
            if (ping_ms >= 0.0)
            {
                printf("%s[✓] Server at %s:%d is ONLINE (Latency: %.2f ms)%s\n",
                       use_colors ? COLOR_GREEN : "", host, port, ping_ms, use_colors ? COLOR_RESET : "");
            }
            else
            {
                printf("%s[✗] Server at %s:%d is UNREACHABLE%s\n",
                       use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
            }
            continue;
        }
        if (strncmp(line, "target ", 7) == 0)
        {
            char new_spec[256];
            int new_port = port;
            if (sscanf(line + 7, "%255s %d", new_spec, &new_port) >= 1)
            {
                char parsed_h[256];
                int p = new_port;
                if (parse_target_spec(new_spec, parsed_h, sizeof(parsed_h), &p))
                {
                    strncpy(host, parsed_h, sizeof(host) - 1);
                    port = p;
                }
                else
                {
                    strncpy(host, new_spec, sizeof(host) - 1);
                    port = new_port;
                }
                double p_ms = ping_server(host, port);
                if (p_ms >= 0.0)
                {
                    printf("%s[✓] Switched target to %s:%d (RTT: %.2f ms)%s\n",
                           use_colors ? COLOR_GREEN : "", host, port, p_ms, use_colors ? COLOR_RESET : "");
                }
                else
                {
                    printf("%s[!] Switched target to %s:%d, but server is currently unreachable.%s\n",
                           use_colors ? COLOR_YELLOW : "", host, port, use_colors ? COLOR_RESET : "");
                }
            }
            continue;
        }

        char task_id[MAX_TASK_ID];
        snprintf(task_id, sizeof(task_id), "t_%d", task_counter++);

        printf("%s[+] Submitting task '%s' to %s:%d...%s\n",
               use_colors ? COLOR_BLUE : "", task_id, host, port, use_colors ? COLOR_RESET : "");

        if (dispatch_task(host, port, task_id, line) != 0)
            continue;

        int exit_code = 0;
        char *output = NULL;
        size_t out_len = 0;

        int res = wait_for_task_completion(host, port, task_id, timeout_sec,
                                           &exit_code, &output, &out_len);
        if (res == 0)
        {
            printf("%s[✓] Completed (exit=%d):%s\n",
                   use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
                   exit_code, use_colors ? COLOR_RESET : "");
            if (out_len > 0 && output)
            {
                fwrite(output, 1, out_len, stdout);
                if (output[out_len - 1] != '\n')
                    putchar('\n');
            }
            free(output);
        }
        else if (res == -3)
        {
            printf("%s[!] Execution timed out after %ds%s\n",
                   use_colors ? COLOR_RED : "", timeout_sec, use_colors ? COLOR_RESET : "");
        }
        else
        {
            printf("%s[!] Failed to retrieve task output from %s:%d%s\n",
                   use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
        }
    }

    return 0;
}
