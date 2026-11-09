CC ?= gcc
CFLAGS = -O3 -Wall -Wextra -pthread -lm

all: matrix_simd

matrix_simd: src/main.c
	$(CC) $(CFLAGS) src/main.c -o matrix_simd

clean:
	rm -f matrix_simd matrix_result.bin matrix_report.json

.PHONY: all clean
