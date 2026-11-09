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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "task.h"
#include "workflow.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_TIMEOUT_SEC 30
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
    safe_copy_str(temp, sizeof(temp), arg);

    char *colon = strchr(temp, ':');
    if (colon)
    {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535)
        {
            *out_port = p;
            safe_copy_str(out_host, max_host, temp);
            return true;
        }
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, temp, &addr) == 1)
    {
        safe_copy_str(out_host, max_host, temp);
        return true;
    }

    if (strcmp(temp, "localhost") == 0 || strstr(temp, ".local"))
    {
        safe_copy_str(out_host, max_host, temp);
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
    hints.ai_family = AF_INET;
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

        struct timeval tv = {.tv_sec = DEFAULT_TIMEOUT_SEC, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

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
    if (sscanf(response, "HTTP/1.1 %d", &status_code) != 1 || status_code != 202)
    {
        fprintf(stderr, "%s[!] Task submission rejected (HTTP %d)%s\n",
                use_colors ? COLOR_RED : "", status_code, use_colors ? COLOR_RESET : "");
        return -1;
    }

    return 0;
}

/* Retrieve task status and output from GET /retrieve/<taskId> */
static int retrieve_task(const char *host, int port, const char *task_id,
                         int *exit_code, int *artifacts_count, char **output_body, size_t *output_len)
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

    if (artifacts_count)
    {
        *artifacts_count = 0;
        char *art_hdr = strcasestr(buf, "X-Artifacts-Count:");
        if (art_hdr)
        {
            sscanf(art_hdr + 18, "%d", artifacts_count);
        }
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

/* Download an artifact file from GET /retrieve/<taskId>/artifact/<filename> */
static int download_artifact(const char *host, int port, const char *task_id, const char *artifact_name)
{
    int sock = connect_to_server(host, port);
    if (sock < 0)
        return -1;

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET /retrieve/%s/artifact/%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        task_id, artifact_name, host, port);

    if (write(sock, request, req_len) != req_len)
    {
        close(sock);
        return -1;
    }

    // Read header first
    size_t cap = 8192;
    size_t total = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        close(sock);
        return -1;
    }

    char *header_end = NULL;
    while (!header_end)
    {
        ssize_t n = read(sock, buf + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
        buf[total] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (!header_end && total >= cap - 1)
        {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(sock); return -1; }
            buf = nb;
        }
    }

    if (!header_end)
    {
        free(buf);
        close(sock);
        return -1;
    }

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.1 %d", &status_code) != 1 || status_code != 200)
    {
        free(buf);
        close(sock);
        return -1;
    }

    size_t content_len = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl)
    {
        content_len = (size_t)strtoull(cl + 15, NULL, 10);
    }

    char *body = header_end + 4;
    size_t body_in_buf = (size_t)(buf + total - body);

    mkdir_p_parent(artifact_name);
    int out_fd = open(artifact_name, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out_fd < 0)
    {
        free(buf);
        close(sock);
        return -1;
    }

    if (body_in_buf > 0)
    {
        write(out_fd, body, body_in_buf);
    }

    size_t written = body_in_buf;
    while (written < content_len)
    {
        char chunk[16384];
        ssize_t n = read(sock, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        write(out_fd, chunk, (size_t)n);
        written += (size_t)n;
    }

    close(out_fd);
    close(sock);
    free(buf);

    chmod(artifact_name, 0755);
    return 0;
}

/* Dispatch binary multi-file envelope workflow */
static int dispatch_workflow(const char *host, int port, const char *manifest_file,
                             int num_files, char **files, int timeout_sec)
{
    printf("%s[+] Packing workflow container...%s\n",
           use_colors ? COLOR_BLUE : "", use_colors ? COLOR_RESET : "");
    printf("%s    • Manifest file: %s%s\n",
           use_colors ? COLOR_CYAN : "", manifest_file, use_colors ? COLOR_RESET : "");
    printf("%s    • Bundled files: %d%s\n",
           use_colors ? COLOR_CYAN : "", num_files, use_colors ? COLOR_RESET : "");

    FILE *mf = fopen(manifest_file, "rb");
    if (!mf)
    {
        fprintf(stderr, "%s[!] Cannot open manifest file '%s': %s%s\n",
                use_colors ? COLOR_RED : "", manifest_file, strerror(errno), use_colors ? COLOR_RESET : "");
        return 1;
    }
    fseek(mf, 0, SEEK_END);
    long mlen = ftell(mf);
    fseek(mf, 0, SEEK_SET);
    char *manifest_json = malloc(mlen + 1);
    if (!manifest_json)
    {
        fclose(mf);
        return 1;
    }
    if (fread(manifest_json, 1, mlen, mf) != (size_t)mlen)
    {
        fclose(mf);
        free(manifest_json);
        return 1;
    }
    manifest_json[mlen] = '\0';
    fclose(mf);

    WorkflowManifest manifest;
    if (parse_workflow_manifest(manifest_json, mlen, &manifest) != 0)
    {
        fprintf(stderr, "%s[!] Failed to parse JSON manifest in '%s'%s\n",
                use_colors ? COLOR_RED : "", manifest_file, use_colors ? COLOR_RESET : "");
        free(manifest_json);
        return 1;
    }

    if (timeout_sec <= 0)
        timeout_sec = manifest.timeout_sec > 0 ? manifest.timeout_sec : DEFAULT_TIMEOUT_SEC;

    printf("%s    • Workflow Name: %s%s\n",
           use_colors ? COLOR_GREEN : "", manifest.name, use_colors ? COLOR_RESET : "");
    printf("%s    • Pre-flight Probe: %d tool(s) | Dependencies: %d | Build Steps: %d%s\n",
           use_colors ? COLOR_GREEN : "", manifest.num_check_tools, manifest.num_deps, manifest.num_build_cmds, use_colors ? COLOR_RESET : "");
    printf("%s    • Required Artifacts: %d%s\n",
           use_colors ? COLOR_GREEN : "", manifest.num_required_artifacts, use_colors ? COLOR_RESET : "");

    // Calculate binary envelope size
    size_t envelope_size = 12 + mlen + 4;
    for (int i = 0; i < num_files; i++)
    {
        struct stat st;
        if (stat(files[i], &st) < 0)
        {
            fprintf(stderr, "%s[!] Source file not found: '%s'%s\n",
                    use_colors ? COLOR_RED : "", files[i], use_colors ? COLOR_RESET : "");
            free(manifest_json);
            return 1;
        }
        size_t path_len = strlen(files[i]);
        if (path_len >= MAX_PATH_LEN)
        {
            fprintf(stderr, "%s[!] File path too long: '%s'%s\n",
                    use_colors ? COLOR_RED : "", files[i], use_colors ? COLOR_RESET : "");
            free(manifest_json);
            return 1;
        }
        envelope_size += 2 + path_len + 2 + 8 + (size_t)st.st_size;
    }

    uint8_t *envelope = malloc(envelope_size);
    if (!envelope)
    {
        perror("malloc envelope");
        free(manifest_json);
        return 1;
    }

    // Write Envelope Header
    envelope[0] = 'J';
    envelope[1] = 'U';
    envelope[2] = 'L';
    envelope[3] = 'Y';
    write_uint16_le(envelope + 4, JULY_ENVELOPE_VERSION);
    write_uint16_le(envelope + 6, 0); // flags
    write_uint32_le(envelope + 8, (uint32_t)mlen);
    memcpy(envelope + 12, manifest_json, mlen);
    write_uint32_le(envelope + 12 + mlen, (uint32_t)num_files);

    uint8_t *p = envelope + 12 + mlen + 4;
    for (int i = 0; i < num_files; i++)
    {
        struct stat st;
        stat(files[i], &st);
        const char *rel_name = files[i];
        if (strncmp(rel_name, "./", 2) == 0)
            rel_name += 2;

        uint16_t plen = (uint16_t)strlen(rel_name);
        write_uint16_le(p, plen);
        p += 2;
        memcpy(p, rel_name, plen);
        p += plen;
        write_uint16_le(p, (uint16_t)(st.st_mode & 0777));
        p += 2;
        write_uint64_le(p, (uint64_t)st.st_size);
        p += 8;

        int fd = open(files[i], O_RDONLY);
        if (fd >= 0)
        {
            if (st.st_size > 0)
            {
                read(fd, p, (size_t)st.st_size);
            }
            close(fd);
        }
        p += st.st_size;
    }
    free(manifest_json);

    printf("%s[+] Connecting to %s:%d...%s\n",
           use_colors ? COLOR_BLUE : "", host, port, use_colors ? COLOR_RESET : "");

    int sock = connect_to_server(host, port);
    if (sock < 0)
    {
        fprintf(stderr, "%s[!] Failed to connect to server at %s:%d%s\n",
                use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
        free(envelope);
        return 1;
    }

    char req_headers[512];
    int hlen = snprintf(req_headers, sizeof(req_headers),
        "POST /workflow HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, envelope_size);

    if (write(sock, req_headers, hlen) != hlen)
    {
        perror("write headers");
        close(sock);
        free(envelope);
        return 1;
    }

    size_t written = 0;
    while (written < envelope_size)
    {
        ssize_t w = write(sock, envelope + written, envelope_size - written);
        if (w <= 0)
        {
            if (w < 0 && errno == EINTR) continue;
            perror("write envelope body");
            close(sock);
            free(envelope);
            return 1;
        }
        written += (size_t)w;
    }
    free(envelope);

    char resp_buf[1024];
    size_t total_resp = 0;
    while (total_resp < sizeof(resp_buf) - 1)
    {
        ssize_t n = read(sock, resp_buf + total_resp, sizeof(resp_buf) - 1 - total_resp);
        if (n <= 0)
            break;
        total_resp += (size_t)n;
        resp_buf[total_resp] = '\0';
        if (strstr(resp_buf, "\r\n\r\n"))
            break;
    }
    close(sock);

    if (total_resp == 0)
    {
        fprintf(stderr, "%s[!] Empty response from server on workflow submit%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        return 1;
    }

    int status_code = 0;
    if (sscanf(resp_buf, "HTTP/1.1 %d", &status_code) != 1 || status_code != 202)
    {
        fprintf(stderr, "%s[!] Workflow submission failed (HTTP %d):\n%s%s\n",
                use_colors ? COLOR_RED : "", status_code, resp_buf, use_colors ? COLOR_RESET : "");
        return 1;
    }

    char task_id[MAX_TASK_ID] = {0};
    char *id_hdr = strcasestr(resp_buf, "X-Task-Id:");
    if (id_hdr)
    {
        sscanf(id_hdr + 10, "%63s", task_id);
    }
    if (task_id[0] == '\0')
    {
        fprintf(stderr, "%s[!] No X-Task-Id returned from server%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        return 1;
    }

    printf("%s[✓] Workflow '%s' enqueued successfully! (Task ID: %s)%s\n",
           use_colors ? COLOR_GREEN : "", manifest.name, task_id, use_colors ? COLOR_RESET : "");
    printf("%s[*] Streaming execution logs from server OS Page Cache...%s\n\n",
           use_colors ? COLOR_BLUE : "", use_colors ? COLOR_RESET : "");

    // Poll for completion
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int exit_code = 0;
    int artifacts_count = 0;
    char *log_output = NULL;
    size_t log_len = 0;

    while (1)
    {
        int res = retrieve_task(host, port, task_id, &exit_code, &artifacts_count, &log_output, &log_len);
        if (res == 0)
        {
            // Completed!
            break;
        }
        else if (res == -2)
        {
            usleep(20000); // 20ms
        }
        else if (res < 0)
        {
            fprintf(stderr, "%s[!] Network error retrieving workflow status%s\n",
                    use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
            return 1;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_sec)
        {
            fprintf(stderr, "%s[!] Workflow execution timed out after %ds%s\n",
                    use_colors ? COLOR_RED : "", timeout_sec, use_colors ? COLOR_RESET : "");
            return 124;
        }

        usleep(50000); // 50ms polling
    }

    // Print full execution log
    if (log_len > 0 && log_output)
    {
        fwrite(log_output, 1, log_len, stdout);
        if (log_output[log_len - 1] != '\n')
            putchar('\n');
        free(log_output);
    }

    if (exit_code != 0)
    {
        printf("\n%s[✗] Workflow FAILED with exit code %d%s\n",
               use_colors ? COLOR_RED : "", exit_code, use_colors ? COLOR_RESET : "");
        return exit_code;
    }

    printf("\n%s[✓] Workflow Completed Successfully (Exit Code: 0)!%s\n",
           use_colors ? COLOR_GREEN : "", use_colors ? COLOR_RESET : "");

    // Stage 5: Zero-copy artifact retrieval
    if (manifest.num_required_artifacts > 0)
    {
        printf("%s[*] Downloading %d required artifact(s) via zero-copy sendfile()...%s\n",
               use_colors ? COLOR_CYAN : "", manifest.num_required_artifacts, use_colors ? COLOR_RESET : "");

        for (int i = 0; i < manifest.num_required_artifacts; i++)
        {
            const char *art = manifest.required_artifacts[i];
            printf("%s    ➔ Downloading '%s'...%s ",
                   use_colors ? COLOR_BLUE : "", art, use_colors ? COLOR_RESET : "");
            fflush(stdout);

            if (download_artifact(host, port, task_id, art) == 0)
            {
                struct stat ast;
                stat(art, &ast);
                printf("%s[DONE] (%ld bytes)%s\n",
                       use_colors ? COLOR_GREEN : "", (long)ast.st_size, use_colors ? COLOR_RESET : "");
            }
            else
            {
                printf("%s[FAILED]%s\n",
                       use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
            }
        }
    }

    return 0;
}

static void print_help(const char *prog)
{
    printf("\n%sJuly Task & Workflow Dispatcher%s\n\n",
           use_colors ? COLOR_BOLD : "", use_colors ? COLOR_RESET : "");
    printf("Usage:\n");
    printf("  # Workflow Container Execution:\n");
    printf("  %s [host:port] workflow <manifest.json> [files...]\n", prog);
    printf("  %s workflow <manifest.json> [files...]\n\n", prog);
    printf("  # Shell Command Execution:\n");
    printf("  %s [host:port] \"<command>\"\n", prog);
    printf("  %s [host:port]                     (Interactive REPL targeting host)\n", prog);
    printf("  %s                                 (Interactive REPL default localhost:8080)\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <host>       Server IP or hostname (default: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <port>       Server port (default: %d)\n", DEFAULT_PORT);
    printf("  -t, --timeout <sec>     Execution timeout in seconds (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  -g, --get <taskId>      Retrieve output or artifact status of a task\n");
    printf("  --no-color              Disable ANSI color output\n");
    printf("  --help                  Show this help message\n\n");
    printf("Workflow Example:\n");
    printf("  %s workflow test_build.json main.c Makefile\n", prog);
    printf("  %s 192.168.1.50 workflow build.json src/main.c src/util.h\n\n", prog);
}

int main(int argc, char **argv)
{
    use_colors = isatty(fileno(stdout));

    char host[256];
    safe_copy_str(host, sizeof(host), DEFAULT_HOST);
    int port = DEFAULT_PORT;
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    const char *retrieve_id = NULL;
    char command_buf[MAX_TASK_CMD] = {0};

    // Environment overrides
    const char *env_host = getenv("DISPATCHER_HOST");
    if (env_host) safe_copy_str(host, sizeof(host), env_host);
    const char *env_port = getenv("DISPATCHER_PORT");
    if (env_port) port = atoi(env_port);

    bool host_set_by_pos = false;
    bool workflow_mode = false;
    const char *manifest_file = NULL;
    int workflow_files_start = 0;

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
        else if ((strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc)
        {
            safe_copy_str(host, sizeof(host), argv[++i]);
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
        {
            port = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) && i + 1 < argc)
        {
            timeout_sec = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--get") == 0) && i + 1 < argc)
        {
            retrieve_id = argv[++i];
        }
        else if (strcmp(argv[i], "workflow") == 0)
        {
            workflow_mode = true;
            if (i + 1 < argc)
            {
                manifest_file = argv[++i];
                workflow_files_start = i + 1;
            }
            break;
        }
        else if (argv[i][0] != '-')
        {
            if (!host_set_by_pos && command_buf[0] == '\0')
            {
                char parsed_host[256];
                int parsed_port = port;
                if (parse_target_spec(argv[i], parsed_host, sizeof(parsed_host), &parsed_port))
                {
                    safe_copy_str(host, sizeof(host), parsed_host);
                    port = parsed_port;
                    host_set_by_pos = true;
                    continue;
                }
            }

            if (command_buf[0] != '\0')
            {
                strncat(command_buf, " ", sizeof(command_buf) - strlen(command_buf) - 1);
            }
            strncat(command_buf, argv[i], sizeof(command_buf) - strlen(command_buf) - 1);
        }
    }

    // 1. Workflow Container Dispatch Mode
    if (workflow_mode)
    {
        if (!manifest_file)
        {
            fprintf(stderr, "%s[!] Missing manifest file for workflow! Usage: %s workflow <manifest.json> [files...]%s\n",
                    use_colors ? COLOR_RED : "", argv[0], use_colors ? COLOR_RESET : "");
            return 1;
        }
        int num_files = argc - workflow_files_start;
        if (num_files < 0)
            num_files = 0;
        char **files = num_files > 0 ? &argv[workflow_files_start] : NULL;

        return dispatch_workflow(host, port, manifest_file, num_files, files, timeout_sec);
    }

    // 2. Query/Retrieve Mode
    if (retrieve_id)
    {
        printf("%s[*] Querying task '%s' from %s:%d...%s\n",
               use_colors ? COLOR_BLUE : "", retrieve_id, host, port, use_colors ? COLOR_RESET : "");
        int exit_code = 0;
        int art_count = 0;
        char *output = NULL;
        size_t out_len = 0;
        int res = retrieve_task(host, port, retrieve_id, &exit_code, &art_count, &output, &out_len);
        if (res == 0)
        {
            printf("%s[✓] Task '%s' Completed (Exit Code: %d, Artifacts: %d):%s\n",
                   use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
                   retrieve_id, exit_code, art_count, use_colors ? COLOR_RESET : "");
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
        else
        {
            fprintf(stderr, "%s[!] Task '%s' not found or error.%s\n",
                    use_colors ? COLOR_RED : "", retrieve_id, use_colors ? COLOR_RESET : "");
            return 1;
        }
    }

    // 3. One-Shot Command Execution
    if (command_buf[0] != '\0')
    {
        char task_id[MAX_TASK_ID];
        snprintf(task_id, sizeof(task_id), "cmd_%ld_%d", (long)time(NULL), getpid());

        printf("%s[+] Target: %s:%d | Task: %s%s\n",
               use_colors ? COLOR_CYAN : "", host, port, task_id, use_colors ? COLOR_RESET : "");
        printf("%s[>] Command:%s %s%s%s\n",
               use_colors ? COLOR_BLUE : "", use_colors ? COLOR_RESET : "",
               use_colors ? COLOR_BOLD : "", command_buf, use_colors ? COLOR_RESET : "");

        if (dispatch_task(host, port, task_id, command_buf) != 0)
            return 1;

        int exit_code = 0;
        int art_count = 0;
        char *output = NULL;
        size_t out_len = 0;
        while (1)
        {
            int res = retrieve_task(host, port, task_id, &exit_code, &art_count, &output, &out_len);
            if (res == 0)
                break;
            usleep(25000);
        }

        printf("%s[✓] Completed (exit=%d):%s\n",
               use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
               exit_code, use_colors ? COLOR_RESET : "");
        if (out_len > 0 && output)
        {
            fwrite(output, 1, out_len, stdout);
            if (output[out_len - 1] != '\n')
                putchar('\n');
            free(output);
        }
        return exit_code;
    }

    // 4. Interactive REPL
    printf("%s┌─────────────────────────────────────────────────────────────┐%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│             July Task & Workflow Dispatcher                 │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│  Target: %-49s  │%s\n",
           use_colors ? COLOR_CYAN : "", host, use_colors ? COLOR_RESET : "");
    printf("%s│  Port:   %-49d  │%s\n",
           use_colors ? COLOR_CYAN : "", port, use_colors ? COLOR_RESET : "");
    printf("%s└─────────────────────────────────────────────────────────────┘%s\n\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");

    char line[MAX_TASK_CMD];
    int task_counter = 1;

    while (1)
    {
        printf("%sdispatcher%s(%s%s:%d%s)> ",
               use_colors ? COLOR_GREEN : "", use_colors ? COLOR_RESET : "",
               use_colors ? COLOR_CYAN : "", host, port, use_colors ? COLOR_RESET : "");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';

        if (len == 0)
            continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;

        char task_id[MAX_TASK_ID];
        snprintf(task_id, sizeof(task_id), "t_%d", task_counter++);

        if (dispatch_task(host, port, task_id, line) != 0)
            continue;

        int exit_code = 0;
        int art_count = 0;
        char *output = NULL;
        size_t out_len = 0;
        while (1)
        {
            int res = retrieve_task(host, port, task_id, &exit_code, &art_count, &output, &out_len);
            if (res == 0)
                break;
            usleep(25000);
        }

        printf("%s[✓] (exit=%d):%s\n",
               use_colors ? (exit_code == 0 ? COLOR_GREEN : COLOR_YELLOW) : "",
               exit_code, use_colors ? COLOR_RESET : "");
        if (out_len > 0 && output)
        {
            fwrite(output, 1, out_len, stdout);
            if (output[out_len - 1] != '\n')
                putchar('\n');
            free(output);
        }
    }

    return 0;
}
