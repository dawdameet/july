#include "kernel_abi.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "ramfs.h"
#include "sched.h"
#include "gfx.h"

extern void shell_poll(void);

static volatile uint64_t g_workers_done = 0;

static void busy_spin_yield(uint64_t iters) {
    for (volatile uint64_t i = 0; i < iters; i++) {
        if ((i & 0xFFFFULL) == 0) {
            sched_yield();
        }
    }
}

static void worker_thread(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    task_info_t *self = sched_current();

    for (uint64_t step = 1; step <= 10; step++) {
        void *tmp = kmalloc(256 + id * 64);
        busy_spin_yield(60000ULL + id * 15000ULL);
        if (self) self->work_progress = step * 10;
        if (tmp) kfree(tmp);
    }

    __sync_fetch_and_add(&g_workers_done, 1);
    kprintf("[SCHED] Worker #%llu finished (%llu ticks)\n", id, self ? self->ticks : 0ULL);
}

void kmain(uint64_t mb_magic, uint64_t mb_info_addr) {
    serial_init();
    kprintf("[JULYOS] Booting 64-bit Long Mode Kernel (magic=0x%x)\n", (unsigned)mb_magic);

    pmm_init(mb_info_addr);
    kprintf("[OK] PMM: %llu MB free / %llu MB total\n",
            pmm_free_bytes() / (1024ULL * 1024ULL),
            pmm_total_bytes() / (1024ULL * 1024ULL));

    idt_init();
    pit_init(100);
    kprintf("[OK] IDT 256 vectors + 100Hz PIT active\n");

    kmalloc_init(32);
    if (kmalloc_selftest() != 0) kpanic("kmalloc selftest failed");
    kprintf("[OK] kmalloc/kfree heap coalesce PASS\n");

    ramfs_init();
    kprintf("[OK] RAMFS mounted (/etc/motd, /proc/uptime)\n");

    sched_init();
    sched_spawn("gfx_worker",  worker_thread, (void *)1);
    sched_spawn("heap_stress", worker_thread, (void *)2);
    sched_spawn("lan_sync_t",  worker_thread, (void *)3);
    kprintf("[OK] Spawned 3 preemptive kernel threads\n");

    gfx_init();
    if (gfx_is_active()) {
        kprintf("[OK] BGA VBE 800x600x32 framebuffer active\n");
    }

    /* Enable hardware timer interrupts for true preemptive scheduling */
    arch_sti();

    bool announced = false;
    uint64_t last_draw_tick = 0;

    while (1) {
        uint64_t now = arch_uptime_ticks();
        if (now != last_draw_tick && (now % 5 == 0)) {
            last_draw_tick = now;
            gfx_render_dashboard();
        }

        if (!announced && g_workers_done >= 3 && sched_switch_count() >= 6) {
            gfx_render_dashboard();
            kprintf("[JULYOS] Context switches observed: %llu\n", sched_switch_count());
            kprintf("[JULYOS] DASHBOARD_READY\n");
            kprintf("julyos> ");
            announced = true;
        }

        shell_poll();
        sched_yield();
        arch_hlt();
    }
}

