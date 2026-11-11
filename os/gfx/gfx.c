#include "gfx.h"
#include "font8x16.h"
#include "kernel_abi.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "ramfs.h"
#include "sched.h"
#include "string.h"

#define COL_BG         0x0B1120U
#define COL_CARD       0x131C31U
#define COL_CARD_HDR   0x1A2540U
#define COL_BORDER     0x2A3958U
#define COL_ACCENT     0x38BDF8U
#define COL_GREEN      0x10B981U
#define COL_AMBER      0xF59E0BU
#define COL_ROSE       0xF43F5EU
#define COL_TEXT       0xF1F5F9U
#define COL_MUTED      0x94A3B8U
#define COL_TRACK      0x0F172AU

static uint32_t *g_fb = NULL;
static uint32_t  g_w = 0;
static uint32_t  g_h = 0;
static uint32_t  g_pitch = 0;
static bool      g_gfx_ready = false;

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_w) w = (int)g_w - x;
    if (y + h > (int)g_h) h = (int)g_h - y;
    for (int row = 0; row < h; row++) {
        uint32_t *dst = g_fb + (size_t)(y + row) * g_pitch + (size_t)x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

static void draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static void draw_char_scaled(int x, int y, char c, uint32_t fg, uint32_t bg, int sx, int sy) {
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';
    const uint8_t *glyph = font8x8_ascii[(uint8_t)c - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t pix = (bits & (0x80U >> col)) ? fg : bg;
            if (sx == 1 && sy == 1) {
                int px = x + col, py = y + row;
                if (px >= 0 && py >= 0 && px < (int)g_w && py < (int)g_h) {
                    g_fb[(size_t)py * g_pitch + (size_t)px] = pix;
                }
            } else {
                fill_rect(x + col * sx, y + row * sy, sx, sy, pix);
            }
        }
    }
}

static void draw_str(int x, int y, const char *s, uint32_t fg, uint32_t bg, int sx, int sy) {
    if (!s) return;
    while (*s) {
        draw_char_scaled(x, y, *s, fg, bg, sx, sy);
        x += 8 * sx;
        s++;
    }
}

static void append_str(char *dst, size_t *pos, size_t max, const char *s) {
    while (*s && *pos + 1 < max) dst[(*pos)++] = *s++;
    dst[*pos] = '\0';
}

static void append_uint(char *dst, size_t *pos, size_t max, uint64_t val) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    else {
        while (val > 0) {
            tmp[n++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    while (n > 0 && *pos + 1 < max) dst[(*pos)++] = tmp[--n];
    dst[*pos] = '\0';
}

static void draw_card(int x, int y, int w, int h, const char *title, uint32_t title_col) {
    fill_rect(x, y, w, h, COL_CARD);
    fill_rect(x, y, w, 26, COL_CARD_HDR);
    fill_rect(x, y, 4, 26, title_col);
    draw_rect_outline(x, y, w, h, COL_BORDER);
    fill_rect(x, y + 26, w, 1, COL_BORDER);
    draw_str(x + 12, y + 9, title, title_col, COL_CARD_HDR, 1, 1);
}

static void draw_progress_bar(int x, int y, int w, int h, uint64_t pct, uint32_t fill_col) {
    if (pct > 100) pct = 100;
    fill_rect(x, y, w, h, COL_TRACK);
    draw_rect_outline(x, y, w, h, COL_BORDER);
    int inner_w = (int)(((uint64_t)(w - 2) * pct) / 100ULL);
    if (inner_w > 0) {
        fill_rect(x + 1, y + 1, inner_w, h - 2, fill_col);
    }
}

void gfx_init(void) {
    if (vbe_init(800, 600, 32) == 0 && vbe_framebuffer() != NULL) {
        g_fb = vbe_framebuffer();
        g_w = vbe_width();
        g_h = vbe_height();
        g_pitch = vbe_pitch_pixels();
        g_gfx_ready = true;
        fill_rect(0, 0, (int)g_w, (int)g_h, COL_BG);
    }
}

bool gfx_is_active(void) {
    return g_gfx_ready;
}

void gfx_render_dashboard(void) {
    if (!g_gfx_ready) return;

    char line[96];
    size_t pos;

    /* 1. Top Status Bar */
    fill_rect(0, 0, (int)g_w, 46, COL_CARD_HDR);
    fill_rect(0, 46, (int)g_w, 2, COL_ACCENT);

    draw_str(16, 15, "JULYOS x86_64", COL_ACCENT, COL_CARD_HDR, 1, 2);
    draw_str(154, 11, "LONG MODE 64-BIT  |  BGA 800x600x32  |  2-AGENT KERNEL", COL_TEXT, COL_CARD_HDR, 1, 1);

    uint64_t ticks = arch_uptime_ticks();
    uint32_t hz = arch_tick_hz() ? arch_tick_hz() : 100;
    uint64_t ms = (ticks * 1000ULL) / hz;
    pos = 0;
    append_str(line, &pos, sizeof(line), "PIT: ");
    append_uint(line, &pos, sizeof(line), hz);
    append_str(line, &pos, sizeof(line), "Hz  |  UPTIME: ");
    append_uint(line, &pos, sizeof(line), ms);
    append_str(line, &pos, sizeof(line), " ms  |  CTX SW: ");
    append_uint(line, &pos, sizeof(line), sched_switch_count());
    append_str(line, &pos, sizeof(line), "   ");
    draw_str(154, 27, line, COL_MUTED, COL_CARD_HDR, 1, 1);

    /* 2. Top-Left Card: Physical & Heap Memory */
    draw_card(16, 62, 376, 252, "[ PHYSICAL & HEAP MEMORY MAP ]", COL_GREEN);

    uint64_t total_mb = pmm_total_bytes() / (1024ULL * 1024ULL);
    uint64_t free_mb  = pmm_free_bytes()  / (1024ULL * 1024ULL);
    uint64_t used_kb  = pmm_used_bytes()  / 1024ULL;
    uint64_t pmm_pct  = pmm_total_bytes() ? (pmm_used_bytes() * 100ULL) / pmm_total_bytes() : 0;

    pos = 0;
    append_str(line, &pos, sizeof(line), "PMM RAM: ");
    append_uint(line, &pos, sizeof(line), free_mb);
    append_str(line, &pos, sizeof(line), " / ");
    append_uint(line, &pos, sizeof(line), total_mb);
    append_str(line, &pos, sizeof(line), " MB free (");
    append_uint(line, &pos, sizeof(line), used_kb);
    append_str(line, &pos, sizeof(line), " KB used)");
    draw_str(30, 100, line, COL_TEXT, COL_CARD, 1, 1);
    draw_progress_bar(30, 114, 348, 14, pmm_pct < 4 ? 4 : pmm_pct, COL_GREEN);

    kmalloc_stats_t hs;
    kmalloc_get_stats(&hs);
    uint64_t heap_pct = hs.total_heap_bytes ? (hs.used_heap_bytes * 100ULL) / hs.total_heap_bytes : 0;

    pos = 0;
    append_str(line, &pos, sizeof(line), "kmalloc: ");
    append_uint(line, &pos, sizeof(line), hs.used_heap_bytes / 1024ULL);
    append_str(line, &pos, sizeof(line), " / ");
    append_uint(line, &pos, sizeof(line), hs.total_heap_bytes / 1024ULL);
    append_str(line, &pos, sizeof(line), " KB  (allocs:");
    append_uint(line, &pos, sizeof(line), hs.alloc_count);
    append_str(line, &pos, sizeof(line), " frees:");
    append_uint(line, &pos, sizeof(line), hs.free_count);
    append_str(line, &pos, sizeof(line), ")");
    draw_str(30, 140, line, COL_TEXT, COL_CARD, 1, 1);
    draw_progress_bar(30, 154, 348, 14, heap_pct < 5 ? 5 : heap_pct, COL_ACCENT);

    draw_str(30, 180, "Live Page / Block Allocation Matrix:", COL_MUTED, COL_CARD, 1, 1);
    /* 16 cols x 5 rows = 80 blocks visualizing heap & PMM activity */
    uint64_t active_tiles = hs.total_heap_bytes ? (hs.used_heap_bytes * 80ULL) / hs.total_heap_bytes : 12;
    if (active_tiles < 10) active_tiles = 10;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 16; c++) {
            int idx = r * 16 + c;
            int bx = 30 + c * 22;
            int by = 196 + r * 20;
            uint32_t col = COL_TRACK;
            if ((uint64_t)idx < active_tiles) {
                col = (idx % 5 == 0) ? COL_AMBER : COL_GREEN;
            } else if ((uint64_t)idx < active_tiles + 8) {
                col = COL_ACCENT;
            }
            fill_rect(bx, by, 18, 16, col);
            draw_rect_outline(bx, by, 18, 16, COL_BORDER);
        }
    }

    /* 3. Top-Right Card: Preemptive Kernel Threads */
    draw_card(408, 62, 376, 252, "[ PREEMPTIVE KERNEL THREADS (IRQ0) ]", COL_ACCENT);

    task_info_t snap[MAX_TASKS];
    size_t ntasks = sched_snapshot(snap, 6);
    for (size_t i = 0; i < ntasks; i++) {
        int ty = 100 + (int)i * 34;
        const char *st_str = "RDY";
        uint32_t st_col = COL_ACCENT;
        if (snap[i].state == TASK_RUNNING) { st_str = "RUN"; st_col = COL_GREEN; }
        else if (snap[i].state == TASK_DONE) { st_str = "FIN"; st_col = COL_MUTED; }

        fill_rect(422, ty - 2, 32, 14, st_col);
        draw_str(426, ty + 1, st_str, COL_BG, st_col, 1, 1);

        pos = 0;
        append_str(line, &pos, sizeof(line), "T");
        append_uint(line, &pos, sizeof(line), snap[i].tid);
        append_str(line, &pos, sizeof(line), " ");
        append_str(line, &pos, sizeof(line), snap[i].name);
        draw_str(462, ty + 1, line, COL_TEXT, COL_CARD, 1, 1);

        pos = 0;
        append_uint(line, &pos, sizeof(line), snap[i].work_progress);
        append_str(line, &pos, sizeof(line), "% (");
        append_uint(line, &pos, sizeof(line), snap[i].ticks);
        append_str(line, &pos, sizeof(line), "t)  ");
        draw_str(672, ty + 1, line, COL_MUTED, COL_CARD, 1, 1);

        uint32_t bar_col = (i == 1) ? COL_GREEN : ((i == 2) ? COL_AMBER : ((i == 3) ? COL_ROSE : COL_ACCENT));
        draw_progress_bar(422, ty + 16, 348, 9, snap[i].work_progress, bar_col);
    }

    /* 4. Bottom-Left Card: RAMFS Explorer */
    draw_card(16, 328, 376, 256, "[ RAMFS EXPLORER (/etc & /proc) ]", COL_AMBER);

    ramfs_dirent_t ents[8];
    size_t nf = ramfs_list(ents, 8);
    for (size_t i = 0; i < nf && i < 6; i++) {
        int fy = 366 + (int)i * 18;
        draw_str(30, fy, ents[i].is_dynamic ? "[DYN]" : "[REG]", ents[i].is_dynamic ? COL_ACCENT : COL_GREEN, COL_CARD, 1, 1);
        draw_str(78, fy, ents[i].name, COL_TEXT, COL_CARD, 1, 1);
        pos = 0;
        append_uint(line, &pos, sizeof(line), ents[i].size);
        append_str(line, &pos, sizeof(line), " B");
        draw_str(310, fy, line, COL_MUTED, COL_CARD, 1, 1);
    }

    fill_rect(28, 480, 352, 90, COL_TRACK);
    draw_rect_outline(28, 480, 352, 90, COL_BORDER);
    draw_str(38, 490, "> cat /etc/motd", COL_AMBER, COL_TRACK, 1, 1);

    char fbuf[96];
    if (ramfs_read("/etc/motd", fbuf, sizeof(fbuf)) > 0) {
        for (size_t k = 0; fbuf[k]; k++) if (fbuf[k] == '\n') fbuf[k] = '\0';
        draw_str(38, 506, fbuf, COL_TEXT, COL_TRACK, 1, 1);
    }
    draw_str(38, 528, "> cat /proc/uptime", COL_AMBER, COL_TRACK, 1, 1);
    if (ramfs_read("/proc/uptime", fbuf, sizeof(fbuf)) > 0) {
        for (size_t k = 0; fbuf[k]; k++) if (fbuf[k] == '\n') fbuf[k] = '\0';
        draw_str(38, 544, fbuf, COL_GREEN, COL_TRACK, 1, 1);
    }

    /* 5. Bottom-Right Card: Live Kernel Serial Console */
    draw_card(408, 328, 376, 256, "[ KERNEL SERIAL CONSOLE (COM1) ]", COL_ROSE);
    fill_rect(420, 362, 352, 210, COL_TRACK);
    draw_rect_outline(420, 362, 352, 210, COL_BORDER);

    size_t lcnt = klog_line_count();
    size_t max_vis = 12;
    size_t first = lcnt > max_vis ? (lcnt - max_vis) : 0;
    for (size_t i = first; i < lcnt; i++) {
        const char *txt = klog_get_line(i);
        char clipped[43];
        strncpy(clipped, txt, 42);
        clipped[42] = '\0';
        uint32_t lcol = COL_TEXT;
        if (strncmp(clipped, "[OK]", 4) == 0) lcol = COL_GREEN;
        else if (strncmp(clipped, "[JULYOS]", 8) == 0) lcol = COL_ACCENT;
        else if (strncmp(clipped, "[SCHED]", 7) == 0) lcol = COL_AMBER;
        draw_str(428, 372 + (int)(i - first) * 16, clipped, lcol, COL_TRACK, 1, 1);
    }
}

