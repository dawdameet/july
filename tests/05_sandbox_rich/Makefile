CC ?= gcc
CFLAGS = -Wall -Wextra -O2

all: sandbox_probe

sandbox_probe: src/main.c
	$(CC) $(CFLAGS) src/main.c -o sandbox_probe

clean:
	rm -f sandbox_probe sandbox_report.json

.PHONY: all clean

