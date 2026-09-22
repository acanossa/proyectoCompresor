CC ?= gcc

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE=700

.PHONY: all clean

all: compresor compresor_paralelo compresor_concurrente comparador

compresor: huffman.c
	$(CC) $(CFLAGS) -o $@ $<

compresor_paralelo: huffman_paralelo.c
	$(CC) $(CFLAGS) -o $@ $<

compresor_concurrente: huffman_concurrente.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

comparador: comparador.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f compresor compresor_paralelo compresor_concurrente comparador
