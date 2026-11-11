CC      := gcc
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-x86_64

CFLAGS  := -m64 -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -Iinclude
ASFLAGS := -m64 -Iinclude
LDFLAGS := -m elf_x86_64 -n -z max-page-size=0x1000 -T linker.ld

ASM_SRCS := boot/boot.S arch/x86_64/isr.S
C_SRCS   := arch/x86_64/idt.c \
            drivers/serial.c \
            drivers/vbe.c \
            mm/pmm.c \
            mm/kmalloc.c \
            lib/string.c \
            lib/kprintf.c \
            fs/ramfs.c \
            kernel/sched.c \
            kernel/shell.c \
            kernel/kmain.c \
            gfx/gfx.c

OBJS := $(ASM_SRCS:.S=.o) $(C_SRCS:.c=.o)

.PHONY: all clean qemu-demo

all: julyos.elf

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

julyos64.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

julyos.elf: julyos64.elf
	$(OBJCOPY) -O elf32-i386 $< $@

qemu-demo: julyos.elf
	@python3 run_qemu_demo.py

clean:
	rm -f $(OBJS) julyos64.elf julyos.elf serial_boot.log julyos_screen.ppm julyos_screen.png /tmp/julyos_mon.sock

