#include "kernel_abi.h"

#define COM1_PORT 0x3F8

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80); /* Enable DLAB */
    outb(COM1_PORT + 0, 0x01); /* 115200 baud (divisor low) */
    outb(COM1_PORT + 1, 0x00); /* Divisor high */
    outb(COM1_PORT + 3, 0x03); /* 8 bits, no parity, 1 stop bit */
    outb(COM1_PORT + 2, 0xC7); /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

void serial_putc(char c) {
    while ((inb(COM1_PORT + 5) & 0x20) == 0) {
        /* spin until THR empty */
    }
    outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s) {
    if (!s) return;
    while (*s) {
        serial_putc(*s++);
    }
}

int serial_try_getc(void) {
    if ((inb(COM1_PORT + 5) & 0x01) == 0) {
        return -1;
    }
    return (int)inb(COM1_PORT);
}

void arch_qemu_exit(int code) {
    outw(0x604, 0x2000);          /* QEMU ACPI shutdown */
    outw(0xB004, 0x2000);         /* Bochs/old QEMU shutdown */
    outb(0xF4, (uint8_t)code);    /* isa-debug-exit fallback */
    while (1) {
        arch_cli();
        arch_hlt();
    }
}

