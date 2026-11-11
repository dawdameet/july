#include "kernel_abi.h"
#include "string.h"

#define PMM_MAX_BYTES (512ULL * 1024ULL * 1024ULL)
#define PMM_MAX_PAGES (PMM_MAX_BYTES / PAGE_SIZE)

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

/* 1 bit per 4 KiB page: 1 = used/reserved, 0 = free */
static uint64_t bitmap[PMM_MAX_PAGES / 64];
static uint64_t g_total_pages = 0;
static uint64_t g_used_pages  = 0;

static void bitmap_set(uint64_t page) {
    if (page < PMM_MAX_PAGES) {
        uint64_t mask = 1ULL << (page % 64);
        if (!(bitmap[page / 64] & mask)) {
            bitmap[page / 64] |= mask;
            g_used_pages++;
        }
    }
}

static void bitmap_clear(uint64_t page) {
    if (page < PMM_MAX_PAGES) {
        uint64_t mask = 1ULL << (page % 64);
        if (bitmap[page / 64] & mask) {
            bitmap[page / 64] &= ~mask;
            if (g_used_pages > 0) g_used_pages--;
        }
    }
}

static bool bitmap_test(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return true;
    return (bitmap[page / 64] & (1ULL << (page % 64))) != 0;
}

void pmm_init(uint64_t mb_info_addr) {
    memset(bitmap, 0xFF, sizeof(bitmap));
    g_total_pages = PMM_MAX_PAGES;
    g_used_pages  = PMM_MAX_PAGES;

    uint64_t max_addr = 128ULL * 1024ULL * 1024ULL;
    const mb_info_t *mb = (const mb_info_t *)(uintptr_t)mb_info_addr;

    if (mb && (mb->flags & (1U << 6)) && mb->mmap_length > 0) {
        uint64_t cur = (uint64_t)mb->mmap_addr;
        uint64_t end = cur + (uint64_t)mb->mmap_length;
        max_addr = 0;

        while (cur < end) {
            const mb_mmap_entry_t *e = (const mb_mmap_entry_t *)(uintptr_t)cur;
            if (e->type == 1 && e->len > 0) {
                uint64_t r_start = e->addr;
                uint64_t r_end   = e->addr + e->len;
                if (r_end > PMM_MAX_BYTES) r_end = PMM_MAX_BYTES;
                if (r_end > max_addr) max_addr = r_end;

                uint64_t first_p = (r_start + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t last_p  = r_end / PAGE_SIZE;
                for (uint64_t p = first_p; p < last_p; p++) {
                    bitmap_clear(p);
                }
            }
            cur += e->size + sizeof(e->size);
        }
    } else {
        /* Fallback: assume 128 MiB usable starting at 2 MiB */
        for (uint64_t p = (2ULL * 1024ULL * 1024ULL) / PAGE_SIZE; p < max_addr / PAGE_SIZE; p++) {
            bitmap_clear(p);
        }
    }

    if (max_addr == 0) max_addr = 128ULL * 1024ULL * 1024ULL;
    g_total_pages = max_addr / PAGE_SIZE;
    if (g_total_pages > PMM_MAX_PAGES) g_total_pages = PMM_MAX_PAGES;

    /* Reserve low 2 MiB + full kernel image + multiboot info */
    uint64_t kend = (uint64_t)(uintptr_t)&_kernel_end;
    uint64_t reserve_end = kend > 0x200000ULL ? kend : 0x200000ULL;
    uint64_t reserve_pages = (reserve_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = 0; p < reserve_pages; p++) {
        bitmap_set(p);
    }

    /* Recompute accurate used count bounded by g_total_pages */
    g_used_pages = 0;
    for (uint64_t p = 0; p < g_total_pages; p++) {
        if (bitmap_test(p)) g_used_pages++;
    }
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t p = 0; p < g_total_pages; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            return p * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t p = phys_addr / PAGE_SIZE;
    if (p > 0 && p < g_total_pages) {
        bitmap_clear(p);
    }
}

uint64_t pmm_total_bytes(void) {
    return g_total_pages * PAGE_SIZE;
}

uint64_t pmm_used_bytes(void) {
    return g_used_pages * PAGE_SIZE;
}

uint64_t pmm_free_bytes(void) {
    return (g_total_pages > g_used_pages ? (g_total_pages - g_used_pages) : 0) * PAGE_SIZE;
}

