#ifndef JULYOS_KERNEL_ABI_H
#define JULYOS_KERNEL_ABI_H

#include "types.h"

#define PAGE_SIZE          4096ULL
#define MULTIBOOT_MAGIC    0x2BADB002U

/* ---------------- Inline x86_64 Port I/O & CPU Helpers ---------------- */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void arch_cli(void) {
    __asm__ volatile ("cli");
}

static inline void arch_sti(void) {
    __asm__ volatile ("sti");
}

static inline void arch_hlt(void) {
    __asm__ volatile ("hlt");
}

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* ---------------- Multiboot1 Info Structures ---------------- */

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) mb_info_t;

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type; /* 1 = available RAM */
} __attribute__((packed)) mb_mmap_entry_t;

/* ---------------- x86_64 Interrupt Stack Frame ----------------
 * Exact stack layout pushed by arch/x86_64/isr.S before calling C / scheduler:
 * Lower memory addresses at top (RSP points to r15).
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) interrupt_frame_t;

/* ---------------- HAL Provided by Fedora Agent ---------------- */

/* Drivers: COM1 Serial (0x3F8) & ACPI Poweroff */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
int  serial_try_getc(void);          /* -1 if no byte ready, else 0..255 */
void arch_qemu_exit(int code);       /* Exits QEMU cleanly (ACPI 0x604 / 0xB004 + hlt loop) */

/* Interrupts & PIT Timer (vectors: 0..31 exceptions, 32=IRQ0 PIT, 128=0x80 cooperative yield) */
void idt_init(void);
void pit_init(uint32_t hz);
uint64_t arch_uptime_ticks(void);
uint32_t arch_tick_hz(void);

/* Physical Memory Manager (4 KiB pages) */
void     pmm_init(uint64_t mb_info_addr);
uint64_t pmm_alloc_page(void);       /* Returns 4K-aligned phys addr, or 0 on OOM */
void     pmm_free_page(uint64_t phys_addr);
uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_free_bytes(void);

/* Bochs/QEMU BGA Linear Framebuffer (32-bit ARGB) */
int       vbe_init(uint32_t width, uint32_t height, uint32_t bpp); /* 0 = OK, -1 = fail */
uint32_t *vbe_framebuffer(void);
uint32_t  vbe_width(void);
uint32_t  vbe_height(void);
uint32_t  vbe_pitch_pixels(void);

/* ---------------- Kernel Provided by Mac Agent ---------------- */

void     kmain(uint64_t mb_magic, uint64_t mb_info_addr);
uint64_t sched_on_tick(uint64_t current_rsp); /* Called from IRQ0 (vector 32) & yield (0x80) */

#endif /* JULYOS_KERNEL_ABI_H */

