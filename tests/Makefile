CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread

all: sys_bench

sys_bench: src/main.c
	$(CC) $(CFLAGS) src/main.c -o sys_bench

clean:
	rm -f sys_bench benchmark_report.json

.PHONY: all clean

