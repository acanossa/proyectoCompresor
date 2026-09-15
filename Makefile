CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: compresor

compresor: huffman.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f compresor