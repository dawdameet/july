#ifndef JULYOS_KMALLOC_H
#define JULYOS_KMALLOC_H

#include "types.h"

typedef struct {
    uint64_t total_heap_bytes;
    uint64_t used_heap_bytes;
    uint64_t free_heap_bytes;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t block_count;
} kmalloc_stats_t;

void  kmalloc_init(uint64_t initial_pages);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  kmalloc_get_stats(kmalloc_stats_t *out);
int   kmalloc_selftest(void);

#endif /* JULYOS_KMALLOC_H */

