#include "kmalloc.h"
#include "kernel_abi.h"
#include "string.h"

#define HEAP_MAGIC 0x4A484541504BULL /* "JHEAPK" */

typedef struct heap_block {
    uint64_t magic;
    size_t   size; /* usable payload size */
    bool     free;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_head = NULL;
static uint64_t g_total_heap_bytes = 0;
static uint64_t g_alloc_count = 0;
static uint64_t g_free_count = 0;

static size_t align16(size_t n) {
    return (n + 15ULL) & ~15ULL;
}

static heap_block_t *heap_grow(size_t min_bytes) {
    size_t needed = align16(min_bytes) + sizeof(heap_block_t);
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 4) pages = 4;

    /* Allocate contiguous pages when possible; otherwise link non-contiguous chunks */
    uint64_t first = pmm_alloc_page();
    if (!first) return NULL;

    size_t got_pages = 1;
    while (got_pages < pages) {
        uint64_t next_p = pmm_alloc_page();
        if (!next_p) break;
        if (next_p == first + got_pages * PAGE_SIZE) {
            got_pages++;
        } else {
            /* Non-contiguous: free back and settle for what we have if enough, else keep separate */
            pmm_free_page(next_p);
            break;
        }
    }

    size_t chunk_bytes = got_pages * PAGE_SIZE;
    g_total_heap_bytes += chunk_bytes;

    heap_block_t *blk = (heap_block_t *)(uintptr_t)first;
    blk->magic = HEAP_MAGIC;
    blk->size  = chunk_bytes - sizeof(heap_block_t);
    blk->free  = true;
    blk->next  = NULL;
    blk->prev  = NULL;

    if (!heap_head) {
        heap_head = blk;
    } else {
        heap_block_t *tail = heap_head;
        while (tail->next) tail = tail->next;
        tail->next = blk;
        blk->prev = tail;
    }
    return blk;
}

void kmalloc_init(uint64_t initial_pages) {
    heap_head = NULL;
    g_total_heap_bytes = 0;
    g_alloc_count = 0;
    g_free_count = 0;

    uint64_t remaining = initial_pages ? initial_pages : 16;
    while (remaining > 0) {
        size_t chunk = remaining > 16 ? 16 : (size_t)remaining;
        if (!heap_grow(chunk * PAGE_SIZE - sizeof(heap_block_t))) break;
        remaining -= chunk;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) size = 16;
    size = align16(size);

    for (int pass = 0; pass < 2; pass++) {
        for (heap_block_t *cur = heap_head; cur; cur = cur->next) {
            if (cur->magic != HEAP_MAGIC) continue;
            if (cur->free && cur->size >= size) {
                if (cur->size >= size + sizeof(heap_block_t) + 16) {
                    heap_block_t *split = (heap_block_t *)((uint8_t *)(cur + 1) + size);
                    split->magic = HEAP_MAGIC;
                    split->size  = cur->size - size - sizeof(heap_block_t);
                    split->free  = true;
                    split->next  = cur->next;
                    split->prev  = cur;
                    if (cur->next) cur->next->prev = split;
                    cur->next = split;
                    cur->size = size;
                }
                cur->free = false;
                g_alloc_count++;
                return (void *)(cur + 1);
            }
        }
        if (!heap_grow(size)) break;
    }
    return NULL;
}

static bool is_adjacent(const heap_block_t *a, const heap_block_t *b) {
    if (!a || !b) return false;
    return (uint8_t *)(a + 1) + a->size == (uint8_t *)b;
}

void kfree(void *ptr) {
    if (!ptr) return;
    heap_block_t *blk = ((heap_block_t *)ptr) - 1;
    if (blk->magic != HEAP_MAGIC || blk->free) return;

    blk->free = true;
    g_free_count++;

    if (blk->next && blk->next->free && is_adjacent(blk, blk->next)) {
        heap_block_t *nxt = blk->next;
        blk->size += sizeof(heap_block_t) + nxt->size;
        blk->next = nxt->next;
        if (nxt->next) nxt->next->prev = blk;
    }

    if (blk->prev && blk->prev->free && is_adjacent(blk->prev, blk)) {
        heap_block_t *prv = blk->prev;
        prv->size += sizeof(heap_block_t) + blk->size;
        prv->next = blk->next;
        if (blk->next) blk->next->prev = prv;
    }
}

void kmalloc_get_stats(kmalloc_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->total_heap_bytes = g_total_heap_bytes;
    out->alloc_count = g_alloc_count;
    out->free_count = g_free_count;

    for (heap_block_t *cur = heap_head; cur; cur = cur->next) {
        if (cur->magic != HEAP_MAGIC) break;
        out->block_count++;
        if (cur->free) out->free_heap_bytes += cur->size;
        else           out->used_heap_bytes += cur->size;
    }
}

int kmalloc_selftest(void) {
    uint8_t *a = (uint8_t *)kmalloc(128);
    uint8_t *b = (uint8_t *)kmalloc(256);
    uint8_t *c = (uint8_t *)kmalloc(128);
    if (!a || !b || !c) return -1;

    memset(a, 0xAA, 128);
    memset(b, 0xBB, 256);
    memset(c, 0xCC, 128);

    kfree(a);
    kfree(b);
    /* Coalesced block >= 384 bytes should now hold 320 bytes */
    uint8_t *d = (uint8_t *)kmalloc(320);
    if (!d) return -2;
    memset(d, 0xDD, 320);

    if (c[0] != 0xCC || c[127] != 0xCC) return -3;
    kfree(c);
    kfree(d);
    return 0;
}

