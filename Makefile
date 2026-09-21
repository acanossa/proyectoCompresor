CC ?= gcc

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: compresor compresor_paralelo compresor_concurrente

compresor: huffman.c
	$(CC) $(CFLAGS) -o $@ $<

compresor_paralelo: huffman_paralelo.c
	$(CC) $(CFLAGS) -o $@ $<

compresor_concurrente: huffman_concurrente.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f compresor compresor_paralelo compresor_concurrente