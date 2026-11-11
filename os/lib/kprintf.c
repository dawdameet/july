#include "kprintf.h"
#include "kernel_abi.h"
#include "string.h"

#define KLOG_MAX_LINES 24
#define KLOG_LINE_LEN  80

static char klog_ring[KLOG_MAX_LINES][KLOG_LINE_LEN];
static size_t klog_head = 0;   /* next line index to write (monotonic) */
static size_t klog_col  = 0;

static void klog_push_char(char c) {
    size_t slot = klog_head % KLOG_MAX_LINES;
    if (c == '\n') {
        klog_ring[slot][klog_col] = '\0';
        klog_head++;
        klog_col = 0;
        size_t next_slot = klog_head % KLOG_MAX_LINES;
        klog_ring[next_slot][0] = '\0';
        return;
    }
    if (c == '\r') return;
    if (klog_col < KLOG_LINE_LEN - 1) {
        klog_ring[slot][klog_col++] = c;
        klog_ring[slot][klog_col] = '\0';
    }
}

static void kputc(char c) {
    serial_putc(c);
    klog_push_char(c);
}

static void kputs(const char *s) {
    if (!s) s = "(null)";
    while (*s) kputc(*s++);
}

static void kprint_uint(uint64_t val, int base, bool upper, int min_width, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        while (val > 0) {
            buf[idx++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }
    while (idx < min_width) {
        kputc(pad);
        min_width--;
    }
    while (idx > 0) {
        kputc(buf[--idx]);
    }
}

void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            kputc(fmt[i]);
            continue;
        }
        i++;
        if (!fmt[i]) break;

        char pad = ' ';
        if (fmt[i] == '0') {
            pad = '0';
            i++;
        }
        int width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }

        bool is_long = false;
        while (fmt[i] == 'l') {
            is_long = true;
            i++;
        }

        switch (fmt[i]) {
            case '%':
                kputc('%');
                break;
            case 'c':
                kputc((char)__builtin_va_arg(ap, int));
                break;
            case 's':
                kputs(__builtin_va_arg(ap, const char *));
                break;
            case 'd':
            case 'i': {
                int64_t v = is_long ? __builtin_va_arg(ap, int64_t)
                                    : (int64_t)__builtin_va_arg(ap, int);
                if (v < 0) {
                    kputc('-');
                    v = -v;
                }
                kprint_uint((uint64_t)v, 10, false, width, pad);
                break;
            }
            case 'u': {
                uint64_t v = is_long ? __builtin_va_arg(ap, uint64_t)
                                     : (uint64_t)__builtin_va_arg(ap, unsigned int);
                kprint_uint(v, 10, false, width, pad);
                break;
            }
            case 'x': {
                uint64_t v = is_long ? __builtin_va_arg(ap, uint64_t)
                                     : (uint64_t)__builtin_va_arg(ap, unsigned int);
                kprint_uint(v, 16, false, width, pad);
                break;
            }
            case 'X': {
                uint64_t v = is_long ? __builtin_va_arg(ap, uint64_t)
                                     : (uint64_t)__builtin_va_arg(ap, unsigned int);
                kprint_uint(v, 16, true, width, pad);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)__builtin_va_arg(ap, void *);
                kputs("0x");
                kprint_uint(v, 16, false, 16, '0');
                break;
            }
            default:
                kputc('%');
                kputc(fmt[i]);
                break;
        }
    }

    __builtin_va_end(ap);
}

void kpanic(const char *msg) {
    arch_cli();
    kprintf("\n[PANIC] %s\n", msg ? msg : "fatal kernel error");
    arch_qemu_exit(1);
    while (1) arch_hlt();
}

size_t klog_line_count(void) {
    size_t total = klog_col > 0 ? (klog_head + 1) : klog_head;
    return total < KLOG_MAX_LINES ? total : KLOG_MAX_LINES;
}

const char *klog_get_line(size_t idx) {
    size_t cnt = klog_line_count();
    if (idx >= cnt) return "";
    size_t total = klog_col > 0 ? (klog_head + 1) : klog_head;
    size_t start = total > KLOG_MAX_LINES ? (total - KLOG_MAX_LINES) : 0;
    return klog_ring[(start + idx) % KLOG_MAX_LINES];
}

