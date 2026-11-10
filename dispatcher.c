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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/uio.h>
#include <dirent.h>
#include <fnmatch.h>

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
static char g_auth_token[256] = {0};

static inline int append_auth_header(char *buf, size_t max_buf)
{
    if (g_auth_token[0] != '\0')
    {
        return snprintf(buf, max_buf, "X-July-Token: %s\r\n", g_auth_token);
    }
    buf[0] = '\0';
    return 0;
}

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

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

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

    char auth_hdr[300] = {0};
    append_auth_header(auth_hdr, sizeof(auth_hdr));

    char headers[512];
    int header_len = snprintf(headers, sizeof(headers),
        "POST /task HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        host, port, payload_len, auth_hdr);

    int sock = connect_to_server(host, port);
    if (sock < 0)
    {
        fprintf(stderr, "%s[!] Failed to connect to server at %s:%d%s\n",
                use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
        free(payload);
        return -1;
    }

    // Vectored I/O: send headers and binary payload in single atomic syscall
    struct iovec iov[2];
    iov[0].iov_base = headers;
    iov[0].iov_len = (size_t)header_len;
    iov[1].iov_base = payload;
    iov[1].iov_len = payload_len;

    size_t total_to_write = (size_t)header_len + payload_len;
    size_t total_written = 0;
    while (total_written < total_to_write)
    {
        ssize_t w = writev(sock, iov, 2);
        if (w <= 0)
        {
            if (w < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            close(sock);
            free(payload);
            return -1;
        }
        total_written += (size_t)w;
        if ((size_t)w >= iov[0].iov_len)
        {
            size_t rem = (size_t)w - iov[0].iov_len;
            iov[0].iov_len = 0;
            iov[1].iov_base = (char *)iov[1].iov_base + rem;
            iov[1].iov_len -= rem;
        }
        else
        {
            iov[0].iov_base = (char *)iov[0].iov_base + w;
            iov[0].iov_len -= (size_t)w;
        }
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
        if (status_code == 401)
        {
            fprintf(stderr, "%s[!] 401 Unauthorized: Remote worker requires authentication! Set JULY_TOKEN or pass --token <token>%s\n",
                    use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        }
        else
        {
            fprintf(stderr, "%s[!] Task submission rejected (HTTP %d)%s\n",
                    use_colors ? COLOR_RED : "", status_code, use_colors ? COLOR_RESET : "");
        }
        return -1;
    }

    return 0;
}

/* Retrieve task status and output with persistent connection reuse */
static int retrieve_task_conn(int *sock_ptr, const char *host, int port, const char *task_id,
                              int *exit_code, int *artifacts_count, char **output_body, size_t *output_len)
{
    int sock = -1;
    if (sock_ptr && *sock_ptr >= 0)
    {
        sock = *sock_ptr;
    }
    else
    {
        sock = connect_to_server(host, port);
        if (sock < 0)
            return -1;
        if (sock_ptr)
            *sock_ptr = sock;
    }

    char auth_hdr[300] = {0};
    append_auth_header(auth_hdr, sizeof(auth_hdr));

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET /retrieve/%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s"
        "Connection: keep-alive\r\n"
        "\r\n",
        task_id, host, port, auth_hdr);

    if (write(sock, request, req_len) != req_len)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        // Reconnect once on broken persistent socket
        sock = connect_to_server(host, port);
        if (sock < 0) return -1;
        if (sock_ptr) *sock_ptr = sock;
        if (write(sock, request, req_len) != req_len)
        {
            close(sock);
            if (sock_ptr) *sock_ptr = -1;
            return -1;
        }
    }

    size_t capacity = RESPONSE_BUF_SIZE;
    size_t total_read = 0;
    char *buf = malloc(capacity);
    if (!buf)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    char *header_end = NULL;
    while (!header_end)
    {
        if (total_read >= capacity - 1)
        {
            capacity *= 2;
            char *new_buf = realloc(buf, capacity);
            if (!new_buf)
            {
                free(buf);
                close(sock);
                if (sock_ptr) *sock_ptr = -1;
                return -1;
            }
            buf = new_buf;
        }

        ssize_t n = read(sock, buf + total_read, capacity - 1 - total_read);
        if (n <= 0)
            break;
        total_read += (size_t)n;
        buf[total_read] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
    }

    if (!header_end)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.1 %d", &status_code) != 1)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    bool server_wants_close = false;
    char *conn_hdr = strcasestr(buf, "Connection:");
    if (conn_hdr && conn_hdr < header_end && strncasecmp(conn_hdr + 11, " close", 6) == 0)
    {
        server_wants_close = true;
    }

    if (status_code == 202)
    {
        free(buf);
        if (server_wants_close || !sock_ptr)
        {
            close(sock);
            if (sock_ptr) *sock_ptr = -1;
        }
        return 1; // Still running
    }

    if (status_code == 401)
    {
        fprintf(stderr, "%s[!] 401 Unauthorized: Invalid token or authentication required%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -3;
    }

    if (status_code == 404)
    {
        free(buf);
        if (server_wants_close || !sock_ptr)
        {
            close(sock);
            if (sock_ptr) *sock_ptr = -1;
        }
        return -2; // Not found
    }

    if (status_code != 200)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    *exit_code = 0;
    char *code_hdr = strcasestr(buf, "X-Task-Exit-Code:");
    if (code_hdr && code_hdr < header_end)
    {
        sscanf(code_hdr + 17, "%d", exit_code);
    }

    if (artifacts_count)
    {
        *artifacts_count = 0;
        char *art_hdr = strcasestr(buf, "X-Artifacts-Count:");
        if (art_hdr && art_hdr < header_end)
        {
            sscanf(art_hdr + 18, "%d", artifacts_count);
        }
    }

    size_t content_len = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < header_end)
    {
        content_len = (size_t)strtoull(cl + 15, NULL, 10);
    }

    char *body_start = header_end + 4;
    size_t body_already_read = (size_t)(buf + total_read - body_start);

    // Read remaining body up to Content-Length
    while (body_already_read < content_len)
    {
        if (total_read >= capacity - 1)
        {
            size_t body_offset = (size_t)(body_start - buf);
            capacity *= 2;
            char *nb = realloc(buf, capacity);
            if (!nb)
            {
                free(buf);
                close(sock);
                if (sock_ptr) *sock_ptr = -1;
                return -1;
            }
            buf = nb;
            body_start = buf + body_offset;
        }
        size_t needed = content_len - body_already_read;
        size_t space = capacity - 1 - total_read;
        size_t to_read = needed < space ? needed : space;
        ssize_t n = read(sock, buf + total_read, to_read);
        if (n <= 0) break;
        total_read += (size_t)n;
        body_already_read += (size_t)n;
    }

    char *out = malloc(body_already_read + 1);
    if (out)
    {
        memcpy(out, body_start, body_already_read);
        out[body_already_read] = '\0';
        *output_body = out;
        *output_len = body_already_read;
    }

    free(buf);

    if (server_wants_close || !sock_ptr)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
    }
    return 0; // Completed
}

static int retrieve_task(const char *host, int port, const char *task_id,
                         int *exit_code, int *artifacts_count, char **output_body, size_t *output_len)
{
    return retrieve_task_conn(NULL, host, port, task_id, exit_code, artifacts_count, output_body, output_len);
}

/* Download an artifact file with persistent socket reuse */
static int download_artifact_conn(int *sock_ptr, const char *host, int port, const char *task_id, const char *artifact_name)
{
    int sock = -1;
    if (sock_ptr && *sock_ptr >= 0)
    {
        sock = *sock_ptr;
    }
    else
    {
        sock = connect_to_server(host, port);
        if (sock < 0)
            return -1;
        if (sock_ptr)
            *sock_ptr = sock;
    }

    char auth_hdr[300] = {0};
    append_auth_header(auth_hdr, sizeof(auth_hdr));

    char request[512];
    int req_len = snprintf(request, sizeof(request),
        "GET /retrieve/%s/artifact/%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s"
        "Connection: keep-alive\r\n"
        "\r\n",
        task_id, artifact_name, host, port, auth_hdr);

    if (write(sock, request, req_len) != req_len)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        sock = connect_to_server(host, port);
        if (sock < 0) return -1;
        if (sock_ptr) *sock_ptr = sock;
        if (write(sock, request, req_len) != req_len)
        {
            close(sock);
            if (sock_ptr) *sock_ptr = -1;
            return -1;
        }
    }

    // Read header first
    size_t cap = 8192;
    size_t total = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
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
            if (!nb) { free(buf); close(sock); if (sock_ptr) *sock_ptr = -1; return -1; }
            buf = nb;
        }
    }

    if (!header_end)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    int status_code = 0;
    if (sscanf(buf, "HTTP/1.1 %d", &status_code) != 1 || status_code != 200)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
        return -1;
    }

    size_t content_len = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < header_end)
    {
        content_len = (size_t)strtoull(cl + 15, NULL, 10);
    }

    bool server_wants_close = false;
    char *conn_hdr = strcasestr(buf, "Connection:");
    if (conn_hdr && conn_hdr < header_end && strncasecmp(conn_hdr + 11, " close", 6) == 0)
    {
        server_wants_close = true;
    }

    char *body = header_end + 4;
    size_t body_in_buf = (size_t)(buf + total - body);
    if (body_in_buf > content_len) body_in_buf = content_len;

    mkdir_p_parent(artifact_name);
    int out_fd = open(artifact_name, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out_fd < 0)
    {
        free(buf);
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
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
        size_t to_read = sizeof(chunk);
        if (content_len - written < to_read) to_read = content_len - written;
        ssize_t n = read(sock, chunk, to_read);
        if (n <= 0)
            break;
        write(out_fd, chunk, (size_t)n);
        written += (size_t)n;
    }

    close(out_fd);
    free(buf);

    if (server_wants_close || !sock_ptr)
    {
        close(sock);
        if (sock_ptr) *sock_ptr = -1;
    }

    chmod(artifact_name, 0755);
    return 0;
}

__attribute__((unused)) static int download_artifact(const char *host, int port, const char *task_id, const char *artifact_name)
{
    return download_artifact_conn(NULL, host, port, task_id, artifact_name);
}

/* Dispatch raw binary multi-file envelope */
static int dispatch_envelope(const char *host, int port, const char *manifest_json, size_t mlen,
                             const WorkflowManifest *manifest, int num_files, const char **files,
                             uint16_t envelope_flags, int timeout_sec)
{
    // Calculate binary envelope size
    size_t envelope_size = 12 + mlen + 4;
    for (int i = 0; i < num_files; i++)
    {
        struct stat st;
        if (stat(files[i], &st) < 0)
        {
            fprintf(stderr, "%s[!] Source file not found: '%s'%s\n",
                    use_colors ? COLOR_RED : "", files[i], use_colors ? COLOR_RESET : "");
            return 1;
        }
        size_t path_len = strlen(files[i]);
        if (path_len >= MAX_PATH_LEN)
        {
            fprintf(stderr, "%s[!] File path too long: '%s'%s\n",
                    use_colors ? COLOR_RED : "", files[i], use_colors ? COLOR_RESET : "");
            return 1;
        }
        envelope_size += 2 + path_len + 2 + 8 + (size_t)st.st_size;
    }

    uint8_t *envelope = malloc(envelope_size);
    if (!envelope)
    {
        perror("malloc envelope");
        return 1;
    }

    // Write Envelope Header
    envelope[0] = 'J';
    envelope[1] = 'U';
    envelope[2] = 'L';
    envelope[3] = 'Y';
    write_uint16_le(envelope + 4, JULY_ENVELOPE_VERSION);
    write_uint16_le(envelope + 6, envelope_flags); // flags: workspace, delta, clean
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
                ssize_t rd = read(fd, p, (size_t)st.st_size);
                (void)rd;
            }
            close(fd);
        }
        p += st.st_size;
    }

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

    char auth_hdr[300] = {0};
    append_auth_header(auth_hdr, sizeof(auth_hdr));

    char req_headers[512];
    int hlen = snprintf(req_headers, sizeof(req_headers),
        "POST /workflow HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: keep-alive\r\n"
        "\r\n",
        host, port, envelope_size, auth_hdr);

    // Vectored I/O: send headers and envelope in single atomic syscall
    struct iovec iov[2];
    iov[0].iov_base = req_headers;
    iov[0].iov_len = (size_t)hlen;
    iov[1].iov_base = (void *)envelope;
    iov[1].iov_len = envelope_size;

    size_t total_to_write = (size_t)hlen + envelope_size;
    size_t total_written = 0;
    while (total_written < total_to_write)
    {
        ssize_t w = writev(sock, iov, 2);
        if (w <= 0)
        {
            if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            perror("writev envelope");
            close(sock);
            free(envelope);
            return 1;
        }
        total_written += (size_t)w;
        if ((size_t)w >= iov[0].iov_len)
        {
            size_t rem = (size_t)w - iov[0].iov_len;
            iov[0].iov_len = 0;
            iov[1].iov_base = (char *)iov[1].iov_base + rem;
            iov[1].iov_len -= rem;
        }
        else
        {
            iov[0].iov_base = (char *)iov[0].iov_base + w;
            iov[0].iov_len -= (size_t)w;
        }
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

    if (total_resp == 0)
    {
        fprintf(stderr, "%s[!] Empty response from server on workflow submit%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        close(sock);
        return 1;
    }

    int status_code = 0;
    if (sscanf(resp_buf, "HTTP/1.1 %d", &status_code) != 1 || status_code != 202)
    {
        if (status_code == 401)
        {
            fprintf(stderr, "%s[!] 401 Unauthorized: Remote worker requires authentication! Set JULY_TOKEN or pass --token <token>%s\n",
                    use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        }
        else
        {
            fprintf(stderr, "%s[!] Workflow submission failed (HTTP %d):\n%s%s\n",
                    use_colors ? COLOR_RED : "", status_code, resp_buf, use_colors ? COLOR_RESET : "");
        }
        close(sock);
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
        close(sock);
        return 1;
    }

    printf("%s[✓] Workflow '%s' enqueued successfully! (Task ID: %s)%s\n",
           use_colors ? COLOR_GREEN : "", manifest->name, task_id, use_colors ? COLOR_RESET : "");
    printf("%s[*] Streaming execution logs from server OS Page Cache...%s\n\n",
           use_colors ? COLOR_BLUE : "", use_colors ? COLOR_RESET : "");

    // Keep the warm connection open for status polling
    int poll_sock = sock;
    useconds_t poll_sleep_us = 1000; // start at 1ms sub-millisecond adaptive backoff

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int exit_code = 0;
    int artifacts_count = 0;
    char *log_output = NULL;
    size_t log_len = 0;

    while (1)
    {
        int res = retrieve_task_conn(&poll_sock, host, port, task_id, &exit_code, &artifacts_count, &log_output, &log_len);
        if (res == 0)
        {
            break;
        }
        else if (res == 1 || res == -2) // 202 Still running or 404
        {
            usleep(poll_sleep_us);
            if (poll_sleep_us < 8000) poll_sleep_us *= 2;
        }
        else
        {
            fprintf(stderr, "%s[!] Network error retrieving workflow status%s\n",
                    use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
            if (poll_sock >= 0) close(poll_sock);
            return 1;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout_sec)
        {
            fprintf(stderr, "%s[!] Workflow execution timed out after %ds%s\n",
                    use_colors ? COLOR_RED : "", timeout_sec, use_colors ? COLOR_RESET : "");
            if (poll_sock >= 0) close(poll_sock);
            return 124;
        }
    }

    // Print full execution log
    if (log_len > 0 && log_output)
    {
        fwrite(log_output, 1, log_len, stdout);
        if (log_output[log_len - 1] != '\n')
            putchar('\n');
        free(log_output);
    }

    if (poll_sock >= 0)
    {
        close(poll_sock);
        poll_sock = -1;
    }

    if (exit_code != 0)
    {
        printf("\n%s[✗] Workflow FAILED with exit code %d%s\n",
               use_colors ? COLOR_RED : "", exit_code, use_colors ? COLOR_RESET : "");
        return exit_code;
    }

    printf("\n%s[✓] Workflow Completed Successfully (Exit Code: 0)!%s\n",
           use_colors ? COLOR_GREEN : "", use_colors ? COLOR_RESET : "");

    // Zero-copy artifact retrieval
    if (manifest->num_required_artifacts > 0)
    {
        printf("%s[*] Downloading %d required artifact(s) via zero-copy sendfile()...%s\n",
               use_colors ? COLOR_CYAN : "", manifest->num_required_artifacts, use_colors ? COLOR_RESET : "");

        for (int i = 0; i < manifest->num_required_artifacts; i++)
        {
            const char *art = manifest->required_artifacts[i];
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

    int ret = dispatch_envelope(host, port, manifest_json, (size_t)mlen, &manifest,
                                num_files, (const char **)files, JULY_FLAG_NONE, timeout_sec);
    free(manifest_json);
    return ret;
}

/* ---------------- Project Workspace & CAS Engine ---------------- */

typedef struct {
    char project[MAX_WORKFLOW_NAME];
    char target[256];
    int num_build_cmds;
    char build_cmds[MAX_BUILD_CMDS][MAX_CMD_LEN];
    int num_artifacts;
    char artifacts[MAX_ARTIFACTS][MAX_PATH_LEN];
    int num_ignores;
    char ignores[32][64];
} JulyProjectConfig;

#define MAX_PROJECT_FILES 2048
#define JULY_CACHE_MAGIC  0x4A434143 // "JCAC"

typedef struct {
    char rel_path[MAX_PATH_LEN];
    uint64_t mtime;
    uint64_t size;
    uint64_t hash;
} ProjectCacheEntry;

static int parse_julyconfig(const char *json, size_t len, JulyProjectConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    safe_copy_str(cfg->project, sizeof(cfg->project), "project");
    safe_copy_str(cfg->target, sizeof(cfg->target), "127.0.0.1:8080");

    // Default ignores
    safe_copy_str(cfg->ignores[0], sizeof(cfg->ignores[0]), ".git*");
    safe_copy_str(cfg->ignores[1], sizeof(cfg->ignores[1]), "*.o");
    safe_copy_str(cfg->ignores[2], sizeof(cfg->ignores[2]), ".july*");
    safe_copy_str(cfg->ignores[3], sizeof(cfg->ignores[3]), "*.dSYM");
    safe_copy_str(cfg->ignores[4], sizeof(cfg->ignores[4]), ".DS_Store");
    cfg->num_ignores = 5;

    const char *p = json;
    const char *end = json + len;

    p = skip_json_ws(p, end);
    if (p >= end || *p != '{') return -1;
    p++;

    while (p < end)
    {
        p = skip_json_ws(p, end);
        if (p < end && *p == '}') { p++; break; }

        char key[64];
        if (!parse_json_string(&p, end, key, sizeof(key))) break;
        p = skip_json_ws(p, end);
        if (p >= end || *p != ':') break;
        p++;
        p = skip_json_ws(p, end);

        if (strcmp(key, "project") == 0 || strcmp(key, "name") == 0)
        {
            parse_json_string(&p, end, cfg->project, sizeof(cfg->project));
        }
        else if (strcmp(key, "target") == 0)
        {
            parse_json_string(&p, end, cfg->target, sizeof(cfg->target));
        }
        else if (strcmp(key, "build") == 0)
        {
            if (p < end && *p == '[')
            {
                char tmp[MAX_BUILD_CMDS][MAX_CMD_LEN];
                int cnt = 0;
                parse_json_string_array(&p, end, tmp, &cnt, MAX_BUILD_CMDS);
                cfg->num_build_cmds = cnt;
                for (int i = 0; i < cnt; i++)
                    safe_copy_str(cfg->build_cmds[i], sizeof(cfg->build_cmds[i]), tmp[i]);
            }
            else
            {
                char cmd[MAX_CMD_LEN];
                if (parse_json_string(&p, end, cmd, sizeof(cmd)))
                {
                    safe_copy_str(cfg->build_cmds[0], sizeof(cfg->build_cmds[0]), cmd);
                    cfg->num_build_cmds = 1;
                }
            }
        }
        else if (strcmp(key, "artifacts") == 0)
        {
            if (p < end && *p == '[')
            {
                char tmp[MAX_ARTIFACTS][MAX_CMD_LEN];
                int cnt = 0;
                parse_json_string_array(&p, end, tmp, &cnt, MAX_ARTIFACTS);
                cfg->num_artifacts = cnt;
                for (int i = 0; i < cnt; i++)
                    safe_copy_str(cfg->artifacts[i], sizeof(cfg->artifacts[i]), tmp[i]);
            }
        }
        else if (strcmp(key, "ignore") == 0)
        {
            if (p < end && *p == '[')
            {
                char tmp[32][MAX_CMD_LEN];
                int cnt = 0;
                parse_json_string_array(&p, end, tmp, &cnt, 32);
                for (int i = 0; i < cnt && cfg->num_ignores < 32; i++)
                    safe_copy_str(cfg->ignores[cfg->num_ignores++], sizeof(cfg->ignores[0]), tmp[i]);
            }
        }
        else
        {
            p = skip_json_value(p, end);
        }

        p = skip_json_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return 0;
}

static bool find_julyconfig(char *out_path, size_t max_len)
{
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return false;

    char curr[1024];
    safe_copy_str(curr, sizeof(curr), cwd);

    while (curr[0] != '\0')
    {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/.julyconfig", curr);
        if (access(probe, R_OK) == 0)
        {
            safe_copy_str(out_path, max_len, probe);
            return true;
        }

        char *last_slash = strrchr(curr, '/');
        if (!last_slash || last_slash == curr) break;
        *last_slash = '\0';
    }
    return false;
}

static int init_july_project(const char *project_name, const char *target)
{
    if (access(".julyconfig", F_OK) == 0)
    {
        fprintf(stderr, "%s[!] .julyconfig already exists in this directory!%s\n",
                use_colors ? COLOR_YELLOW : "", use_colors ? COLOR_RESET : "");
        return 1;
    }

    char def_name[64] = "my_project";
    if (project_name && project_name[0] != '\0')
    {
        safe_copy_str(def_name, sizeof(def_name), project_name);
    }
    else
    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
        {
            char *base = strrchr(cwd, '/');
            if (base && base[1] != '\0')
                safe_copy_str(def_name, sizeof(def_name), base + 1);
        }
    }

    char def_target[256];
    safe_copy_str(def_target, sizeof(def_target), target ? target : "192.168.1.41:8080");

    FILE *f = fopen(".julyconfig", "w");
    if (!f)
    {
        perror("fopen .julyconfig");
        return 1;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"project\": \"%s\",\n", def_name);
    fprintf(f, "  \"target\": \"%s\",\n", def_target);
    fprintf(f, "  \"build\": [\n");
    fprintf(f, "    \"make -j4\"\n");
    fprintf(f, "  ],\n");
    fprintf(f, "  \"artifacts\": [\n");
    fprintf(f, "    \"app\"\n");
    fprintf(f, "  ],\n");
    fprintf(f, "  \"ignore\": [\n");
    fprintf(f, "    \".git*\",\n");
    fprintf(f, "    \"*.o\",\n");
    fprintf(f, "    \".july*\",\n");
    fprintf(f, "    \"*.dSYM\",\n");
    fprintf(f, "    \".DS_Store\"\n");
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);

    mkdir(".july", 0755);
    FILE *gi = fopen(".july/.gitignore", "w");
    if (gi)
    {
        fprintf(gi, "*\n");
        fclose(gi);
    }

    printf("%s[✓] Initialized July project '%s' in .julyconfig%s\n",
           use_colors ? COLOR_GREEN : "", def_name, use_colors ? COLOR_RESET : "");
    printf("%s    • Target: %s%s\n",
           use_colors ? COLOR_CYAN : "", def_target, use_colors ? COLOR_RESET : "");
    printf("%s    • Run '%s' or '%s build' to dispatch incremental builds.%s\n\n",
           use_colors ? COLOR_BLUE : "", "july", "july", use_colors ? COLOR_RESET : "");
    return 0;
}

static uint64_t hash_file_fnv1a(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint64_t h = 14695981039346656037ULL;
    uint8_t buf[16384];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            h ^= (uint64_t)buf[i];
            h *= 1099511628211ULL;
        }
    }
    close(fd);
    return h;
}

static bool is_path_ignored(const char *rel_path, const char *name, const JulyProjectConfig *cfg)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;
    for (int i = 0; i < cfg->num_artifacts; i++)
    {
        if (strcmp(cfg->artifacts[i], name) == 0 || strcmp(cfg->artifacts[i], rel_path) == 0)
            return true;
    }
    for (int i = 0; i < cfg->num_ignores; i++)
    {
        if (fnmatch(cfg->ignores[i], name, 0) == 0 ||
            fnmatch(cfg->ignores[i], rel_path, 0) == 0)
            return true;
    }
    return false;
}

static void scan_project_dir_rec(const char *base_dir, const char *rel_prefix,
                                 const JulyProjectConfig *cfg,
                                 char files[MAX_PROJECT_FILES][MAX_PATH_LEN], int *count)
{
    char full_dir[1024];
    if (rel_prefix[0])
        snprintf(full_dir, sizeof(full_dir), "%s/%s", base_dir, rel_prefix);
    else
        safe_copy_str(full_dir, sizeof(full_dir), base_dir);

    DIR *d = opendir(full_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        char rel_path[MAX_PATH_LEN];
        if (rel_prefix[0])
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, ent->d_name);
        else
            safe_copy_str(rel_path, sizeof(rel_path), ent->d_name);

        if (is_path_ignored(rel_path, ent->d_name, cfg))
            continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, rel_path);

        struct stat st;
        if (lstat(full_path, &st) < 0) continue;

        if (S_ISDIR(st.st_mode))
        {
            scan_project_dir_rec(base_dir, rel_path, cfg, files, count);
        }
        else if (S_ISREG(st.st_mode))
        {
            if (*count < MAX_PROJECT_FILES)
            {
                safe_copy_str(files[*count], sizeof(files[*count]), rel_path);
                (*count)++;
            }
        }
    }
    closedir(d);
}

static int dispatch_project(const char *config_path, const char *override_host, int override_port, bool force_clean, int timeout_sec)
{
    FILE *f = fopen(config_path, "rb");
    if (!f)
    {
        fprintf(stderr, "%s[!] Cannot open .julyconfig at '%s': %s%s\n",
                use_colors ? COLOR_RED : "", config_path, strerror(errno), use_colors ? COLOR_RESET : "");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long clen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *cfg_json = malloc(clen + 1);
    if (!cfg_json) { fclose(f); return 1; }
    if (fread(cfg_json, 1, clen, f) != (size_t)clen) { fclose(f); free(cfg_json); return 1; }
    cfg_json[clen] = '\0';
    fclose(f);

    JulyProjectConfig cfg;
    if (parse_julyconfig(cfg_json, clen, &cfg) != 0)
    {
        fprintf(stderr, "%s[!] Failed to parse .julyconfig%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        free(cfg_json);
        return 1;
    }
    free(cfg_json);

    // Determine target host and port
    char host[256];
    int port = DEFAULT_PORT;
    char target_str[300];
    if (override_host && override_host[0] != '\0')
    {
        safe_copy_str(host, sizeof(host), override_host);
        port = override_port > 0 ? override_port : DEFAULT_PORT;
        snprintf(target_str, sizeof(target_str), "%s:%d", host, port);
    }
    else
    {
        safe_copy_str(target_str, sizeof(target_str), cfg.target);
        if (!parse_target_spec(target_str, host, sizeof(host), &port))
        {
            safe_copy_str(host, sizeof(host), DEFAULT_HOST);
            port = DEFAULT_PORT;
        }
    }

    // Project root directory
    char root_dir[1024];
    safe_copy_str(root_dir, sizeof(root_dir), config_path);
    char *slash = strrchr(root_dir, '/');
    if (slash) *slash = '\0';
    else safe_copy_str(root_dir, sizeof(root_dir), ".");

    // Scan project files
    static char all_files[MAX_PROJECT_FILES][MAX_PATH_LEN];
    int total_files = 0;
    scan_project_dir_rec(root_dir, "", &cfg, all_files, &total_files);

    // Read cache from .july/cache.bin
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/.july/cache.bin", root_dir);

    static ProjectCacheEntry cached_entries[MAX_PROJECT_FILES];
    int cached_count = 0;
    FILE *cf = fopen(cache_path, "rb");
    if (cf)
    {
        uint32_t cmagic = 0, cver = 0, ccnt = 0;
        if (fread(&cmagic, sizeof(cmagic), 1, cf) == 1 && cmagic == JULY_CACHE_MAGIC &&
            fread(&cver, sizeof(cver), 1, cf) == 1 && cver == 1 &&
            fread(&ccnt, sizeof(ccnt), 1, cf) == 1)
        {
            if (ccnt > MAX_PROJECT_FILES) ccnt = MAX_PROJECT_FILES;
            cached_count = (int)fread(cached_entries, sizeof(ProjectCacheEntry), ccnt, cf);
        }
        fclose(cf);
    }

    // Determine dirty files and build new cache entries
    static ProjectCacheEntry new_entries[MAX_PROJECT_FILES];
    static const char *dirty_files[MAX_PROJECT_FILES];
    int dirty_count = 0;

    for (int i = 0; i < total_files; i++)
    {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", root_dir, all_files[i]);
        struct stat st;
        if (stat(full_path, &st) < 0) continue;

        safe_copy_str(new_entries[i].rel_path, sizeof(new_entries[i].rel_path), all_files[i]);
        new_entries[i].mtime = (uint64_t)st.st_mtime;
        new_entries[i].size = (uint64_t)st.st_size;

        bool dirty = false;
        if (force_clean || cached_count == 0)
        {
            dirty = true;
            new_entries[i].hash = hash_file_fnv1a(full_path);
        }
        else
        {
            int found_idx = -1;
            for (int k = 0; k < cached_count; k++)
            {
                if (strcmp(cached_entries[k].rel_path, all_files[i]) == 0)
                {
                    found_idx = k;
                    break;
                }
            }

            if (found_idx >= 0 &&
                cached_entries[found_idx].mtime == (uint64_t)st.st_mtime &&
                cached_entries[found_idx].size == (uint64_t)st.st_size)
            {
                new_entries[i].hash = cached_entries[found_idx].hash;
                dirty = false;
            }
            else
            {
                new_entries[i].hash = hash_file_fnv1a(full_path);
                if (found_idx < 0 || new_entries[i].hash != cached_entries[found_idx].hash)
                {
                    dirty = true;
                }
            }
        }

        if (dirty)
        {
            dirty_files[dirty_count++] = all_files[i];
        }
    }

    printf("%s┌─────────────────────────────────────────────────────────────┐%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│             July Persistent Project Engine                  │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│  Project: %-49s │%s\n",
           use_colors ? COLOR_CYAN : "", cfg.project, use_colors ? COLOR_RESET : "");
    printf("%s│  Target:  %-49s │%s\n",
           use_colors ? COLOR_CYAN : "", target_str, use_colors ? COLOR_RESET : "");
    printf("%s│  Mode:    %-49s │%s\n",
           use_colors ? COLOR_CYAN : "",
           force_clean ? "Clean Rebuild (All Files Dirty)" :
           (dirty_count == 0 ? "All 100% Cached (Incremental Make)" : "Delta Update (Dirty Only)"),
           use_colors ? COLOR_RESET : "");
    printf("%s└─────────────────────────────────────────────────────────────┘%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");

    printf("%s[+] Scanned %d project file(s): %d dirty, %d cached%s\n",
           use_colors ? COLOR_BLUE : "", total_files, dirty_count, total_files - dirty_count, use_colors ? COLOR_RESET : "");
    if (dirty_count > 0 && dirty_count <= 8)
    {
        for (int i = 0; i < dirty_count; i++)
        {
            printf("%s    ➔ Modified: %s%s\n",
                   use_colors ? COLOR_YELLOW : "", dirty_files[i], use_colors ? COLOR_RESET : "");
        }
    }
    else if (dirty_count > 8)
    {
        printf("%s    ➔ %d files modified (batch transmission)%s\n",
               use_colors ? COLOR_YELLOW : "", dirty_count, use_colors ? COLOR_RESET : "");
    }

    // Synthesize JSON manifest in memory
    char manifest_json[8192];
    int mlen = snprintf(manifest_json, sizeof(manifest_json),
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"build\": [",
        cfg.project);

    for (int i = 0; i < cfg.num_build_cmds; i++)
    {
        mlen += snprintf(manifest_json + mlen, sizeof(manifest_json) - mlen,
            "\"%s\"%s", cfg.build_cmds[i], (i + 1 < cfg.num_build_cmds) ? ", " : "");
    }
    mlen += snprintf(manifest_json + mlen, sizeof(manifest_json) - mlen, "],\n  \"artifacts\": {\n    \"required\": [");
    for (int i = 0; i < cfg.num_artifacts; i++)
    {
        mlen += snprintf(manifest_json + mlen, sizeof(manifest_json) - mlen,
            "\"%s\"%s", cfg.artifacts[i], (i + 1 < cfg.num_artifacts) ? ", " : "");
    }
    mlen += snprintf(manifest_json + mlen, sizeof(manifest_json) - mlen, "]\n  }\n}\n");

    WorkflowManifest manifest;
    if (parse_workflow_manifest(manifest_json, (size_t)mlen, &manifest) != 0)
    {
        fprintf(stderr, "%s[!] Internal error constructing workflow manifest%s\n",
                use_colors ? COLOR_RED : "", use_colors ? COLOR_RESET : "");
        return 1;
    }

    if (timeout_sec <= 0) timeout_sec = DEFAULT_TIMEOUT_SEC;

    uint16_t env_flags = JULY_FLAG_WORKSPACE;
    if (force_clean) env_flags |= JULY_FLAG_CLEAN;
    else if (dirty_count < total_files && cached_count > 0) env_flags |= JULY_FLAG_DELTA;

    char saved_cwd[1024];
    if (getcwd(saved_cwd, sizeof(saved_cwd)))
    {
        chdir(root_dir);
    }

    struct timespec b_start, b_end;
    clock_gettime(CLOCK_MONOTONIC, &b_start);

    int ret = dispatch_envelope(host, port, manifest_json, (size_t)mlen, &manifest,
                                dirty_count, dirty_files, env_flags, timeout_sec);

    clock_gettime(CLOCK_MONOTONIC, &b_end);
    double elapsed = (double)(b_end.tv_sec - b_start.tv_sec) +
                     (double)(b_end.tv_nsec - b_start.tv_nsec) / 1e9;

    chdir(saved_cwd);

    if (ret == 0)
    {
        mkdir_p_parent(cache_path);
        FILE *wcf = fopen(cache_path, "wb");
        if (wcf)
        {
            uint32_t cmagic = JULY_CACHE_MAGIC;
            uint32_t cver = 1;
            uint32_t ccnt = (uint32_t)total_files;
            fwrite(&cmagic, sizeof(cmagic), 1, wcf);
            fwrite(&cver, sizeof(cver), 1, wcf);
            fwrite(&ccnt, sizeof(ccnt), 1, wcf);
            fwrite(new_entries, sizeof(ProjectCacheEntry), total_files, wcf);
            fclose(wcf);
        }

        printf("%s[⚡] Project build finished in %.3f seconds!%s\n\n",
               use_colors ? COLOR_GREEN : "", elapsed, use_colors ? COLOR_RESET : "");
    }
    return ret;
}

static void print_help(const char *prog)
{
    printf("\n%sJuly Task & Workflow Dispatcher%s\n\n",
           use_colors ? COLOR_BOLD : "", use_colors ? COLOR_RESET : "");
    printf("Usage:\n");
    printf("  # Project / CAS Workspace Mode (with .julyconfig):\n");
    printf("  %s init [project_name] [target]    Initialize a new July project (.julyconfig)\n", prog);
    printf("  %s [build|run]                     Incremental build and sync changed files\n", prog);
    printf("  %s clean                           Clean rebuild of persistent workspace\n\n", prog);
    printf("  # Ad-Hoc Workflow Container Execution (No .julyconfig):\n");
    printf("  %s [host:port] workflow <manifest.json> [files...]\n", prog);
    printf("  %s workflow <manifest.json> [files...]\n\n", prog);
    printf("  # Shell Command Execution:\n");
    printf("  %s [host:port] \"<command>\"\n", prog);
    printf("  %s [host:port]                     (Interactive developer console)\n", prog);
    printf("  %s                                 (Interactive console or project build)\n\n", prog);
    printf("Options:\n");
    printf("  -h, --host <host>       Server IP or hostname (default: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <port>       Server port (default: %d)\n", DEFAULT_PORT);
    printf("  -k, --token <token>     Authentication token (or set JULY_TOKEN)\n");
    printf("  -t, --timeout <sec>     Execution timeout in seconds (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  -g, --get <taskId>      Retrieve output or artifact status of a task\n");
    printf("  --clean                 Force clean rebuild of project workspace\n");
    printf("  --no-color              Disable ANSI color output\n");
    printf("  --help                  Show this help message\n\n");
    printf("Workflow Example:\n");
    printf("  %s workflow test_build.json main.c Makefile\n", prog);
    printf("  %s 192.168.1.50 workflow build.json src/main.c src/util.h\n\n", prog);
}

int main(int argc, char **argv)
{
    use_colors = isatty(fileno(stdout));

    if (argc >= 2 && strcmp(argv[1], "init") == 0)
    {
        const char *pname = argc >= 3 ? argv[2] : NULL;
        const char *ptarget = argc >= 4 ? argv[3] : NULL;
        return init_july_project(pname, ptarget);
    }

    char host[256];
    safe_copy_str(host, sizeof(host), DEFAULT_HOST);
    int port = DEFAULT_PORT;
    int timeout_sec = DEFAULT_TIMEOUT_SEC;
    const char *retrieve_id = NULL;
    char command_buf[MAX_TASK_CMD] = {0};
    bool clean_mode = false;

    // Environment overrides
    const char *env_host = getenv("DISPATCHER_HOST");
    if (env_host) safe_copy_str(host, sizeof(host), env_host);
    const char *env_port = getenv("DISPATCHER_PORT");
    if (env_port) port = atoi(env_port);
    const char *env_tok = getenv("JULY_TOKEN");
    if (env_tok) safe_copy_str(g_auth_token, sizeof(g_auth_token), env_tok);

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
        else if (strcmp(argv[i], "--clean") == 0 || strcmp(argv[i], "clean") == 0)
        {
            clean_mode = true;
        }
        else if ((strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--token") == 0) && i + 1 < argc)
        {
            safe_copy_str(g_auth_token, sizeof(g_auth_token), argv[++i]);
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

    // Check if project workspace mode applies (.julyconfig exists)
    char config_path[1024];
    bool has_project = find_julyconfig(config_path, sizeof(config_path));
    if (has_project && !workflow_mode && !retrieve_id)
    {
        if (command_buf[0] == '\0' || strcmp(command_buf, "build") == 0 ||
            strcmp(command_buf, "run") == 0 || strcmp(command_buf, "clean") == 0)
        {
            return dispatch_project(config_path, host_set_by_pos ? host : NULL,
                                    host_set_by_pos ? port : 0,
                                    clean_mode || strcmp(command_buf, "clean") == 0, timeout_sec);
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

    // 4. Interactive REPL / Ambient Developer Console
    printf("%s┌─────────────────────────────────────────────────────────────┐%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│             July Interactive Developer Console              │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s│  Target: %-50s │%s\n",
           use_colors ? COLOR_CYAN : "", host, use_colors ? COLOR_RESET : "");
    printf("%s│  Port:   %-50d │%s\n",
           use_colors ? COLOR_CYAN : "", port, use_colors ? COLOR_RESET : "");
    if (g_auth_token[0] != '\0')
    {
        printf("%s│  Auth:   Token Enabled                                      │%s\n",
               use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    }
    printf("%s│  Type 'help' for commands, 'exit' or Ctrl+D to quit         │%s\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");
    printf("%s└─────────────────────────────────────────────────────────────┘%s\n\n",
           use_colors ? COLOR_CYAN : "", use_colors ? COLOR_RESET : "");

    char line[MAX_TASK_CMD];
    int task_counter = 1;

    while (1)
    {
        printf("%sjuly%s(%s%s:%d%s)> ",
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

        if (strcmp(line, "build") == 0 || strcmp(line, "run") == 0 || strcmp(line, "clean") == 0)
        {
            char cfg_p[1024];
            if (find_julyconfig(cfg_p, sizeof(cfg_p)))
            {
                dispatch_project(cfg_p, host, port, strcmp(line, "clean") == 0, timeout_sec);
                continue;
            }
            else
            {
                printf("%s[!] No .julyconfig found in current directory. Run 'july init' first.%s\n",
                       use_colors ? COLOR_YELLOW : "", use_colors ? COLOR_RESET : "");
                continue;
            }
        }

        if (strncmp(line, "run ", 4) == 0 || strncmp(line, "workflow ", 9) == 0)
        {
            char *args[64];
            int arg_count = 0;
            char *p = line + (line[0] == 'r' ? 4 : 9);
            while (*p && arg_count < 64)
            {
                while (*p == ' ') p++;
                if (!*p) break;
                args[arg_count++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = '\0';
            }
            if (arg_count < 1)
            {
                printf("%s[!] Usage: run <manifest.json> [files...]%s\n",
                       use_colors ? COLOR_YELLOW : "", use_colors ? COLOR_RESET : "");
                continue;
            }
            const char *m_file = args[0];
            int num_f = arg_count - 1;
            char **f_list = num_f > 0 ? &args[1] : NULL;
            dispatch_workflow(host, port, m_file, num_f, f_list, timeout_sec);
            continue;
        }
        else if (strcmp(line, "help") == 0)
        {
            printf("\n%sJuly Interactive Console Commands:%s\n",
                   use_colors ? COLOR_BOLD : "", use_colors ? COLOR_RESET : "");
            printf("  build / run                      Incremental build and sync project (.julyconfig)\n");
            printf("  clean                            Clean rebuild of persistent project workspace\n");
            printf("  run <manifest.json> [files...]   Bundle and dispatch ad-hoc workflow container\n");
            printf("  workflow <manifest.json> [...]   Alias for 'run'\n");
            printf("  status / ping                    Check remote worker health & round-trip latency\n");
            printf("  <command>                        Execute raw remote shell command\n");
            printf("  help                             Display this help menu\n");
            printf("  exit / quit                      Disconnect and exit session cleanly\n\n");
            continue;
        }
        else if (strcmp(line, "status") == 0 || strcmp(line, "ping") == 0)
        {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int s = connect_to_server(host, port);
            if (s >= 0)
            {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                close(s);
                double rtt = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                             (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
                printf("%s[✓] Remote node %s:%d is ONLINE (TCP RTT: %.2f ms)%s\n",
                       use_colors ? COLOR_GREEN : "", host, port, rtt, use_colors ? COLOR_RESET : "");
            }
            else
            {
                printf("%s[!] Remote node %s:%d is OFFLINE or unreachable%s\n",
                       use_colors ? COLOR_RED : "", host, port, use_colors ? COLOR_RESET : "");
            }
            continue;
        }

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
