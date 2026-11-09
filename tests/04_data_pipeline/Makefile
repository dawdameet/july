CC ?= gcc
CFLAGS = -O3 -Wall -Wextra

all: tick_gen

tick_gen: src/main.c
	$(CC) $(CFLAGS) src/main.c -o tick_gen

clean:
	rm -f tick_gen ticks.bin analytics_summary.json

.PHONY: all clean
