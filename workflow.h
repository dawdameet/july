#ifndef WORKFLOW_H
#define WORKFLOW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define JULY_ENVELOPE_MAGIC   0x4A554C59 // "JULY" in ASCII
#define JULY_ENVELOPE_VERSION 1

#define MAX_WORKFLOW_NAME 128
#define MAX_GITHUB_LINK 512
#define MAX_TOOLS 32
#define MAX_TOOL_NAME 64
#define MAX_DEPS 64
#define MAX_BUILD_CMDS 128
#define MAX_CMD_LEN 1024
#define MAX_ARTIFACTS 64
#define MAX_PATH_LEN 256

typedef struct {
    char name[MAX_WORKFLOW_NAME];
    int timeout_sec;
    bool kill_if_timeout;
    bool from_github;
    char github_link[MAX_GITHUB_LINK];

    int num_check_tools;
    char check_tools[MAX_TOOLS][MAX_TOOL_NAME];

    int num_deps;
    char deps[MAX_DEPS][MAX_CMD_LEN];

    int num_build_cmds;
    char build_cmds[MAX_BUILD_CMDS][MAX_CMD_LEN];

    int num_required_artifacts;
    char required_artifacts[MAX_ARTIFACTS][MAX_PATH_LEN];
} WorkflowManifest;

/* ---------------- Little-Endian Helpers ---------------- */

static inline uint16_t read_uint16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_uint32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t read_uint64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (i * 8);
    }
    return v;
}

static inline void write_uint16_le(uint8_t *p, uint16_t val) {
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

static inline void write_uint32_le(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

static inline void write_uint64_le(uint8_t *p, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((val >> (i * 8)) & 0xFF);
    }
}

static inline void safe_copy_str(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) return;
    size_t len = strlen(src);
    if (len >= dest_size) len = dest_size - 1;
    memcpy(dest, src, len);
    dest[len] = '\0';
}

/* ---------------- Directory Utilities ---------------- */

static inline int mkdir_p(const char *dir_path)
{
    if (!dir_path || dir_path[0] == '\0')
        return 0;

    char tmp[1024];
    safe_copy_str(tmp, sizeof(tmp), dir_path);

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
        len--;
    }

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return 0;
}

static inline int mkdir_p_parent(const char *file_path)
{
    if (!file_path)
        return 0;
    char tmp[1024];
    safe_copy_str(tmp, sizeof(tmp), file_path);
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash)
        return 0;
    *last_slash = '\0';
    return mkdir_p(tmp);
}

/* ---------------- Lightweight JSON Manifest Parser ---------------- */

static inline const char *skip_json_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static inline bool parse_json_string(const char **p_in, const char *end, char *out, size_t out_max) {
    const char *p = skip_json_ws(*p_in, end);
    if (p >= end || *p != '"') return false;
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= end) return false;
            char c = *p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            if (i < out_max - 1) out[i++] = c;
            p++;
        } else {
            if (i < out_max - 1) out[i++] = *p;
            p++;
        }
    }
    if (p >= end || *p != '"') return false;
    out[i] = '\0';
    *p_in = p + 1;
    return true;
}

static inline bool parse_json_int(const char **p_in, const char *end, int *out) {
    const char *p = skip_json_ws(*p_in, end);
    if (p >= end) return false;
    char *endptr;
    long val = strtol(p, &endptr, 10);
    if (endptr == p) return false;
    *out = (int)val;
    *p_in = endptr;
    return true;
}

static inline bool parse_json_bool(const char **p_in, const char *end, bool *out) {
    const char *p = skip_json_ws(*p_in, end);
    if (p + 4 <= end && strncmp(p, "true", 4) == 0) {
        *out = true;
        *p_in = p + 4;
        return true;
    }
    if (p + 5 <= end && strncmp(p, "false", 5) == 0) {
        *out = false;
        *p_in = p + 5;
        return true;
    }
    return false;
}

static inline bool parse_json_string_array(const char **p_in, const char *end,
                                           char out_arr[][MAX_CMD_LEN], int *count, int max_count) {
    const char *p = skip_json_ws(*p_in, end);
    if (p >= end || *p != '[') return false;
    p++;
    *count = 0;
    while (p < end) {
        p = skip_json_ws(p, end);
        if (p < end && *p == ']') {
            p++;
            *p_in = p;
            return true;
        }
        if (*count < max_count) {
            char item[MAX_CMD_LEN];
            if (!parse_json_string(&p, end, item, sizeof(item))) return false;
            safe_copy_str(out_arr[*count], MAX_CMD_LEN, item);
            (*count)++;
        } else {
            char dummy[MAX_CMD_LEN];
            if (!parse_json_string(&p, end, dummy, sizeof(dummy))) return false;
        }
        p = skip_json_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return false;
}

static inline const char *skip_json_value(const char *p, const char *end) {
    p = skip_json_ws(p, end);
    if (p >= end) return end;
    if (*p == '"') {
        char dummy[1024];
        const char *tmp = p;
        if (parse_json_string(&tmp, end, dummy, sizeof(dummy))) return tmp;
        return end;
    }
    if (*p == '{') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                char dummy[1024];
                const char *tmp = p;
                if (parse_json_string(&tmp, end, dummy, sizeof(dummy))) { p = tmp; continue; }
            }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') {
                char dummy[1024];
                const char *tmp = p;
                if (parse_json_string(&tmp, end, dummy, sizeof(dummy))) { p = tmp; continue; }
            }
            p++;
        }
        return p;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != '\r') p++;
    return p;
}

static inline int parse_workflow_manifest(const char *json, size_t len, WorkflowManifest *m) {
    memset(m, 0, sizeof(*m));
    m->timeout_sec = 30; // default
    m->kill_if_timeout = true; // default

    const char *end = json + len;
    const char *p = skip_json_ws(json, end);
    if (p >= end || *p != '{') return -1;
    p++;

    while (p < end) {
        p = skip_json_ws(p, end);
        if (p < end && *p == '}') break;

        char key[64];
        if (!parse_json_string(&p, end, key, sizeof(key))) break;
        p = skip_json_ws(p, end);
        if (p >= end || *p != ':') break;
        p++; // skip ':'
        p = skip_json_ws(p, end);

        if (strcmp(key, "name") == 0) {
            parse_json_string(&p, end, m->name, sizeof(m->name));
        } else if (strcmp(key, "options") == 0) {
            if (p < end && *p == '{') {
                p++;
                while (p < end) {
                    p = skip_json_ws(p, end);
                    if (p < end && *p == '}') { p++; break; }
                    char opt_key[64];
                    if (!parse_json_string(&p, end, opt_key, sizeof(opt_key))) break;
                    p = skip_json_ws(p, end);
                    if (p >= end || *p != ':') break;
                    p++;
                    p = skip_json_ws(p, end);
                    if (strcmp(opt_key, "timeout_sec") == 0) {
                        parse_json_int(&p, end, &m->timeout_sec);
                    } else if (strcmp(opt_key, "kill_if_timeout") == 0) {
                        parse_json_bool(&p, end, &m->kill_if_timeout);
                    } else if (strcmp(opt_key, "from_github") == 0) {
                        parse_json_bool(&p, end, &m->from_github);
                    } else if (strcmp(opt_key, "github_link") == 0) {
                        parse_json_string(&p, end, m->github_link, sizeof(m->github_link));
                    } else {
                        p = skip_json_value(p, end);
                    }
                    p = skip_json_ws(p, end);
                    if (p < end && *p == ',') p++;
                }
            } else {
                p = skip_json_value(p, end);
            }
        } else if (strcmp(key, "pre_flight") == 0) {
            if (p < end && *p == '{') {
                p++;
                while (p < end) {
                    p = skip_json_ws(p, end);
                    if (p < end && *p == '}') { p++; break; }
                    char pf_key[64];
                    if (!parse_json_string(&p, end, pf_key, sizeof(pf_key))) break;
                    p = skip_json_ws(p, end);
                    if (p >= end || *p != ':') break;
                    p++;
                    p = skip_json_ws(p, end);
                    if (strcmp(pf_key, "check_tools") == 0) {
                        char tmp[MAX_TOOLS][MAX_CMD_LEN];
                        int cnt = 0;
                        parse_json_string_array(&p, end, tmp, &cnt, MAX_TOOLS);
                        m->num_check_tools = cnt;
                        for (int k = 0; k < cnt; k++) {
                            safe_copy_str(m->check_tools[k], sizeof(m->check_tools[k]), tmp[k]);
                        }
                    } else if (strcmp(pf_key, "deps") == 0) {
                        char tmp[MAX_DEPS][MAX_CMD_LEN];
                        int cnt = 0;
                        parse_json_string_array(&p, end, tmp, &cnt, MAX_DEPS);
                        m->num_deps = cnt;
                        for (int k = 0; k < cnt; k++) {
                            safe_copy_str(m->deps[k], sizeof(m->deps[k]), tmp[k]);
                        }
                    } else {
                        p = skip_json_value(p, end);
                    }
                    p = skip_json_ws(p, end);
                    if (p < end && *p == ',') p++;
                }
            } else {
                p = skip_json_value(p, end);
            }
        } else if (strcmp(key, "build") == 0) {
            char tmp[MAX_BUILD_CMDS][MAX_CMD_LEN];
            int cnt = 0;
            parse_json_string_array(&p, end, tmp, &cnt, MAX_BUILD_CMDS);
            m->num_build_cmds = cnt;
            for (int k = 0; k < cnt; k++) {
                safe_copy_str(m->build_cmds[k], sizeof(m->build_cmds[k]), tmp[k]);
            }
        } else if (strcmp(key, "required_artifacts") == 0) {
            char tmp[MAX_ARTIFACTS][MAX_CMD_LEN];
            int cnt = 0;
            parse_json_string_array(&p, end, tmp, &cnt, MAX_ARTIFACTS);
            m->num_required_artifacts = cnt;
            for (int k = 0; k < cnt; k++) {
                safe_copy_str(m->required_artifacts[k], sizeof(m->required_artifacts[k]), tmp[k]);
            }
        } else {
            p = skip_json_value(p, end);
        }

        p = skip_json_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return 0;
}

/* ---------------- Binary Envelope Unpacker ---------------- */

static inline int unpack_envelope(const uint8_t *data, size_t total_len,
                                   const char *sandbox_dir, WorkflowManifest *out_manifest)
{
    if (total_len < 16)
        return -1;

    uint32_t magic = read_uint32_le(data);
    if (memcmp(data, "JULY", 4) != 0 && magic != JULY_ENVELOPE_MAGIC && magic != 0x594C554A)
        return -1;

    uint16_t version = read_uint16_le(data + 4);
    (void)version;
    uint32_t manifest_len = read_uint32_le(data + 8);

    if (12 + (size_t)manifest_len + 4 > total_len)
        return -1;

    const char *manifest_json = (const char *)(data + 12);
    if (parse_workflow_manifest(manifest_json, manifest_len, out_manifest) != 0)
        return -1;

    uint32_t num_files = read_uint32_le(data + 12 + manifest_len);
    const uint8_t *p = data + 12 + manifest_len + 4;
    const uint8_t *end = data + total_len;

    for (uint32_t i = 0; i < num_files; i++)
    {
        if (p + 2 > end) return -1;
        uint16_t path_len = read_uint16_le(p);
        p += 2;

        if (p + path_len + 2 + 8 > end || path_len >= MAX_PATH_LEN) return -1;
        char rel_path[MAX_PATH_LEN];
        memcpy(rel_path, p, path_len);
        rel_path[path_len] = '\0';
        p += path_len;

        // Path traversal defense
        if (rel_path[0] == '/' || strstr(rel_path, ".."))
            return -1;

        uint16_t mode = read_uint16_le(p);
        p += 2;
        uint64_t file_len = read_uint64_le(p);
        p += 8;

        if (p + file_len > end) return -1;

        char dest_path[1024];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", sandbox_dir, rel_path);
        mkdir_p_parent(dest_path);

        int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, mode ? (mode_t)mode : 0644);
        if (fd < 0) return -1;

        if (file_len > 0)
        {
            size_t written = 0;
            while (written < file_len)
            {
                ssize_t w = write(fd, p + written, file_len - written);
                if (w <= 0)
                {
                    if (w < 0 && errno == EINTR) continue;
                    close(fd);
                    return -1;
                }
                written += (size_t)w;
            }
        }
        close(fd);

        if (mode)
        {
            chmod(dest_path, (mode_t)mode);
        }

        p += file_len;
    }

    return 0;
}

#endif // WORKFLOW_H
