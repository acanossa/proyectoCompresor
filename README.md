# Compresor Huffman

Programa de línea de comandos para Debian 13. Comprime recursivamente los archivos regulares de un directorio usando un árbol de Huffman independiente por archivo. El formato `HUF1` incluye el nombre relativo, tamaño, frecuencias del árbol, cantidad de bits y MD5 de cada archivo.

## Compilar

```sh
make
```

Solo se necesita `gcc` y `make`

## Uso

```sh
./compresor compress <directorio> <archivo.huf>
./compresor extract  <archivo.huf> <directorio>
./compresor_paralelo compress <directorio> <archivo.huf>
./compresor_paralelo extract  <archivo.huf> <directorio>
```
