CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c11

OBJS = math_engine.o string_parser.o hash_table.o telemetry.o main.o
TARGET = sync_app

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

