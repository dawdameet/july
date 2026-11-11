#ifndef JULYOS_RAMFS_H
#define JULYOS_RAMFS_H

#include "types.h"

#define RAMFS_MAX_FILES 32
#define RAMFS_NAME_LEN  64

typedef struct {
    char   name[RAMFS_NAME_LEN];
    size_t size;
    bool   is_dynamic;
} ramfs_dirent_t;

void   ramfs_init(void);
int    ramfs_write(const char *path, const void *data, size_t len);
ssize_t ramfs_read(const char *path, void *buf, size_t max_len);
size_t ramfs_list(ramfs_dirent_t *out, size_t max_out);

#endif /* JULYOS_RAMFS_H */

