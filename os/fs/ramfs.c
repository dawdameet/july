#include "ramfs.h"
#include "kmalloc.h"
#include "kernel_abi.h"
#include "string.h"

typedef struct {
    bool     used;
    char     name[RAMFS_NAME_LEN];
    uint8_t *data;
    size_t   size;
    bool     is_dynamic;
} ramfs_node_t;

static ramfs_node_t fs_nodes[RAMFS_MAX_FILES];

static int find_node(const char *path) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (fs_nodes[i].used && strcmp(fs_nodes[i].name, path) == 0) {
            return i;
        }
    }
    return -1;
}

static size_t format_uint_str(char *buf, uint64_t val) {
    char tmp[32];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    else {
        while (val > 0) {
            tmp[n++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return (size_t)n;
}

void ramfs_init(void) {
    memset(fs_nodes, 0, sizeof(fs_nodes));

    const char *motd = "JulyOS x86_64: built by 2 AIs over LAN\n";
    ramfs_write("/etc/motd", motd, strlen(motd));

    const char *arch = "arch=x86_64 paging=4level-2MiB pit=100Hz\n";
    ramfs_write("/etc/kernel", arch, strlen(arch));

    /* Dynamic pseudo-files */
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!fs_nodes[i].used) {
            fs_nodes[i].used = true;
            strcpy(fs_nodes[i].name, "/proc/uptime");
            fs_nodes[i].is_dynamic = true;
            fs_nodes[i].size = 24;
            break;
        }
    }
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!fs_nodes[i].used) {
            fs_nodes[i].used = true;
            strcpy(fs_nodes[i].name, "/proc/meminfo");
            fs_nodes[i].is_dynamic = true;
            fs_nodes[i].size = 48;
            break;
        }
    }
}

int ramfs_write(const char *path, const void *data, size_t len) {
    if (!path || strlen(path) >= RAMFS_NAME_LEN) return -1;
    int idx = find_node(path);
    if (idx < 0) {
        for (int i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!fs_nodes[i].used) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) return -1;

    if (fs_nodes[idx].data) {
        kfree(fs_nodes[idx].data);
        fs_nodes[idx].data = NULL;
    }

    uint8_t *copy = (uint8_t *)kmalloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, data, len);
    copy[len] = '\0';

    fs_nodes[idx].used = true;
    strcpy(fs_nodes[idx].name, path);
    fs_nodes[idx].data = copy;
    fs_nodes[idx].size = len;
    fs_nodes[idx].is_dynamic = false;
    return 0;
}

ssize_t ramfs_read(const char *path, void *buf, size_t max_len) {
    if (!path || !buf || max_len == 0) return -1;

    if (strcmp(path, "/proc/uptime") == 0) {
        char tmp[64];
        uint64_t ticks = arch_uptime_ticks();
        uint32_t hz = arch_tick_hz() ? arch_tick_hz() : 100;
        size_t pos = 0;
        memcpy(tmp + pos, "ticks=", 6); pos += 6;
        pos += format_uint_str(tmp + pos, ticks);
        memcpy(tmp + pos, " ms=", 4); pos += 4;
        pos += format_uint_str(tmp + pos, (ticks * 1000ULL) / hz);
        tmp[pos++] = '\n';
        size_t cpy = pos < max_len - 1 ? pos : max_len - 1;
        memcpy(buf, tmp, cpy);
        ((char *)buf)[cpy] = '\0';
        return (ssize_t)cpy;
    }

    if (strcmp(path, "/proc/meminfo") == 0) {
        char tmp[96];
        size_t pos = 0;
        memcpy(tmp + pos, "free_kb=", 8); pos += 8;
        pos += format_uint_str(tmp + pos, pmm_free_bytes() / 1024ULL);
        memcpy(tmp + pos, " total_kb=", 10); pos += 10;
        pos += format_uint_str(tmp + pos, pmm_total_bytes() / 1024ULL);
        tmp[pos++] = '\n';
        size_t cpy = pos < max_len - 1 ? pos : max_len - 1;
        memcpy(buf, tmp, cpy);
        ((char *)buf)[cpy] = '\0';
        return (ssize_t)cpy;
    }

    int idx = find_node(path);
    if (idx < 0 || !fs_nodes[idx].data) return -1;
    size_t cpy = fs_nodes[idx].size < max_len - 1 ? fs_nodes[idx].size : max_len - 1;
    memcpy(buf, fs_nodes[idx].data, cpy);
    ((char *)buf)[cpy] = '\0';
    return (ssize_t)cpy;
}

size_t ramfs_list(ramfs_dirent_t *out, size_t max_out) {
    size_t n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES && n < max_out; i++) {
        if (!fs_nodes[i].used) continue;
        strcpy(out[n].name, fs_nodes[i].name);
        out[n].size = fs_nodes[i].size;
        out[n].is_dynamic = fs_nodes[i].is_dynamic;
        n++;
    }
    return n;
}

