# Compresor Huffman

Programa de línea de comandos para Debian 13. Comprime recursivamente los archivos regulares de un directorio usando un árbol de Huffman independiente por archivo. El formato `HUF1` incluye el nombre relativo, tamaño, frecuencias del árbol, cantidad de bits y MD5 de cada archivo.

## Compilar

```sh
make
```

Solo necesita `gcc` y `make`; MD5 está implementado en el propio programa y no requiere OpenSSL.

## Uso

```sh
./compresor compress <directorio> <archivo.huf>
./compresor extract  <archivo.huf> <directorio>
```

La extracción rechaza rutas absolutas y rutas con `..`. Cada archivo se verifica después de escribirlo; si el MD5 no coincide, se elimina el archivo afectado y el comando termina con error. El programa procesa los archivos secuencialmente.