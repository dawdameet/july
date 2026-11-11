#ifndef JULYOS_KPRINTF_H
#define JULYOS_KPRINTF_H

#include "types.h"

void kprintf(const char *fmt, ...);
void kpanic(const char *msg) __attribute__((noreturn));

/* Console ring buffer for rendering latest logs inside graphical dashboard */
size_t klog_line_count(void);
const char *klog_get_line(size_t idx);

#endif /* JULYOS_KPRINTF_H */

