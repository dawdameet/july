CC ?= gcc
CFLAGS = -O3 -Wall -Wextra -pthread

all: kv_engine

kv_engine: src/main.c
	$(CC) $(CFLAGS) src/main.c -o kv_engine

clean:
	rm -f kv_engine wal.bin kv_report.json

.PHONY: all clean
