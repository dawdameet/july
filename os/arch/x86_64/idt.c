#include "kernel_abi.h"
#include "kprintf.h"
#include "string.h"

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

static idt_entry_t idt[256];
static volatile uint64_t g_uptime_ticks = 0;
static uint32_t g_tick_hz = 100;

extern void isr_0(void);  extern void isr_1(void);  extern void isr_2(void);  extern void isr_3(void);
extern void isr_4(void);  extern void isr_5(void);  extern void isr_6(void);  extern void isr_7(void);
extern void isr_8(void);  extern void isr_9(void);  extern void isr_10(void); extern void isr_11(void);
extern void isr_12(void); extern void isr_13(void); extern void isr_14(void); extern void isr_15(void);
extern void isr_16(void); extern void isr_17(void); extern void isr_18(void); extern void isr_19(void);
extern void isr_20(void); extern void isr_21(void); extern void isr_22(void); extern void isr_23(void);
extern void isr_24(void); extern void isr_25(void); extern void isr_26(void); extern void isr_27(void);
extern void isr_28(void); extern void isr_29(void); extern void isr_30(void); extern void isr_31(void);
extern void isr_32(void); extern void isr_128(void);

static void idt_set_gate(int vec, void (*handler)(void), uint8_t attr) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vec].selector    = 0x08;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = attr;
    idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); /* Master -> vector 32 */
    outb(0xA1, 0x28); io_wait(); /* Slave  -> vector 40 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    /* Unmask only IRQ0 (PIT timer); mask all other IRQs */
    outb(0x21, 0xFE);
    outb(0xA1, 0xFF);
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    void (*exc[32])(void) = {
        isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7,
        isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15,
        isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23,
        isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    };
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, exc[i], 0x8E);
    }

    idt_set_gate(32,  isr_32,  0x8E); /* IRQ0 PIT timer */
    idt_set_gate(128, isr_128, 0x8E); /* 0x80 cooperative yield */

    pic_remap();

    idtr_t idtr;
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)(uintptr_t)&idt[0];
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    g_tick_hz = hz;
    uint32_t divisor = 1193182U / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t arch_uptime_ticks(void) {
    return g_uptime_ticks;
}

uint32_t arch_tick_hz(void) {
    return g_tick_hz;
}

uint64_t isr_dispatch(uint64_t rsp) {
    interrupt_frame_t *frame = (interrupt_frame_t *)(uintptr_t)rsp;

    if (frame->int_no == 32) {
        g_uptime_ticks++;
        outb(0x20, 0x20); /* PIC EOI */
        return sched_on_tick(rsp);
    }

    if (frame->int_no == 128) {
        return sched_on_tick(rsp);
    }

    kprintf("\n[EXCEPTION] vec=%llu err=0x%llx rip=%p cr2=0x%llx\n",
            frame->int_no, frame->err_code, (void *)(uintptr_t)frame->rip, read_cr2());
    arch_qemu_exit(1);
    while (1) arch_hlt();
}

