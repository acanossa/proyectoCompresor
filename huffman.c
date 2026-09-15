#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAGIC "HUF1"          // Firma mágica del formato de archivo comprimido
#define MAX_PATH_LEN 65535u

// Estructura para almacenar rutas de archivos a comprimir
typedef struct {
    char *path;
} InputFile;

typedef struct {
    InputFile *items;
    size_t count;
    size_t capacity;
} FileList;

// Nodo del árbol de Huffman
typedef struct {
    uint64_t frequency;  // Suma de frecuencias (nodos internos)
    int left;            // Índice del nodo izquierdo
    int right;           // Índice del nodo derecho
    int symbol;          // Símbolo (0-255) si es hoja, -1 si es nodo interno
} HuffmanNode;

typedef struct {
    HuffmanNode nodes[511];
    int root;
    int count;
} HuffmanTree;

// Contexto MD5: mantiene el estado de la función hash durante el cálculo
typedef struct {
    uint32_t state[4];      // Estado interno del hash
    uint64_t bit_count;     // Cantidad de bits procesados
    unsigned char buffer[64]; // Buffer para acumular datos
    size_t buffer_used;     // Bytes utilizados en el buffer actual
} Md5;

static void md5_init(Md5 *md5);
static void md5_update(Md5 *md5, const unsigned char *data, size_t length);
static void md5_final(Md5 *md5, unsigned char digest[16]);

static void usage(const char *program) {
    fprintf(stderr,
            "Uso:\n"
            "  %s compress <directorio> <archivo.huf>\n"
            "  %s extract  <archivo.huf> <directorio>\n",
            program, program);
}

static int write_bytes(FILE *file, const void *data, size_t length) {
    return fwrite(data, 1, length, file) == length ? 0 : -1;
}

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                              (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u64(FILE *file, uint64_t value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
    return write_bytes(file, bytes, sizeof(bytes));
}

static int read_bytes(FILE *file, void *data, size_t length) {
    return fread(data, 1, length, file) == length ? 0 : -1;
}

static int read_u16(FILE *file, uint16_t *value) {
    unsigned char bytes[2];
    if (read_bytes(file, bytes, sizeof(bytes)) != 0) return -1;
    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return 0;
}

static int read_u32(FILE *file, uint32_t *value) {
    unsigned char bytes[4];
    if (read_bytes(file, bytes, sizeof(bytes)) != 0) return -1;
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 0;
}

static int read_u64(FILE *file, uint64_t *value) {
    unsigned char bytes[8];
    if (read_bytes(file, bytes, sizeof(bytes)) != 0) return -1;
    *value = 0;
    for (int i = 0; i < 8; ++i) *value |= (uint64_t)bytes[i] << (8 * i);
    return 0;
}

static int append_file(FileList *list, const char *path) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        InputFile *new_items = realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) return -1;
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count].path = strdup(path);
    if (list->items[list->count].path == NULL) return -1;
    ++list->count;
    return 0;
}

static void free_file_list(FileList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].path);
    free(list->items);
}

// Recorre recursivamente un directorio y recopila todos los archivos regulares
// (excluye directorios, enlaces simbólicos, etc.)
static int collect_files(const char *directory, const char *relative, FileList *list) {
    char full_path[4096];
    int written = snprintf(full_path, sizeof(full_path), "%s%s%s", directory,
                           relative[0] == '\0' ? "" : "/", relative);
    if (written < 0 || (size_t)written >= sizeof(full_path)) {
        fprintf(stderr, "Ruta demasiado larga: %s\n", full_path);
        return -1;
    }

    // Abre el directorio para lectura
    DIR *dir = opendir(full_path);
    if (dir == NULL) {
        fprintf(stderr, "No se puede abrir %s: %s\n", full_path, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL && result == 0) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child_relative[4096];
        written = snprintf(child_relative, sizeof(child_relative), "%s%s%s", relative,
                           relative[0] == '\0' ? "" : "/", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child_relative)) {
            fprintf(stderr, "Ruta demasiado larga dentro de %s\n", directory);
            result = -1;
            break;
        }
        char child_full[4096];
        written = snprintf(child_full, sizeof(child_full), "%s/%s", directory, child_relative);
        if (written < 0 || (size_t)written >= sizeof(child_full)) {
            result = -1;
            break;
        }
        struct stat info;
        if (lstat(child_full, &info) != 0) {
            fprintf(stderr, "No se puede leer %s: %s\n", child_full, strerror(errno));
            result = -1;
        } else if (S_ISDIR(info.st_mode)) {
            result = collect_files(directory, child_relative, list);
        } else if (S_ISREG(info.st_mode)) {
            result = append_file(list, child_relative);
        }
    }
    closedir(dir);
    return result;
}

// Construye un árbol de Huffman a partir de las frecuencias de los símbolos
// agrupa repetidamente los dos nodos de menor frecuencia
static void tree_build(HuffmanTree *tree, const uint64_t frequencies[256]) {
    int active[511];  // Índices de nodos activos (hojas o padres recientes)
    int active_count = 0;
    tree->count = 0;
    
    // Crear nodos hoja para cada símbolo que aparece en el archivo
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[symbol] == 0) continue;
        tree->nodes[tree->count] = (HuffmanNode){frequencies[symbol], -1, -1, symbol};
        active[active_count++] = tree->count++;
    }
    if (active_count == 0) {
        tree->root = -1;  // Archivo vacío
        return;
    }
    
    // Combinar nodos hasta tener un solo árbol raíz
    while (active_count > 1) {
        int first = 0, second = 1;
        if (tree->nodes[active[second]].frequency < tree->nodes[active[first]].frequency ||
            (tree->nodes[active[second]].frequency == tree->nodes[active[first]].frequency &&
             active[second] < active[first])) {
            int temp = first; first = second; second = temp;
        }
        for (int i = 2; i < active_count; ++i) {
            int node = active[i];
            if (tree->nodes[node].frequency < tree->nodes[active[first]].frequency ||
                (tree->nodes[node].frequency == tree->nodes[active[first]].frequency && node < active[first])) {
                second = first;
                first = i;
            } else if (tree->nodes[node].frequency < tree->nodes[active[second]].frequency ||
                       (tree->nodes[node].frequency == tree->nodes[active[second]].frequency && node < active[second])) {
                second = i;
            }
        }
        // Crear un nodo padre combinando los dos nodos más pequeños
        int first_node = active[first], second_node = active[second];
        tree->nodes[tree->count] = (HuffmanNode){
            tree->nodes[first_node].frequency + tree->nodes[second_node].frequency,
            first_node, second_node, -1};  // -1 --> nodo interno
        active[first] = tree->count++;     // Sustituir el primer nodo por el padre
        active[second] = active[--active_count];  // Eliminar el segundo nodo
    }
    tree->root = active[0];
}

// Genera códigos binarios Huffman recorriendo el árbol de forma recursiva
// 0 para la rama izquierda, 1 para la rama derecha
static void make_codes(const HuffmanTree *tree, int node, char codes[256][256], char *path, int depth) {
    // Caso base: hoja del árbol (símbolo encontrado)
    if (tree->nodes[node].symbol >= 0) {
        if (depth == 0) path[depth++] = '0';  // archivo con un único símbolo
        path[depth] = '\0';
        strcpy(codes[tree->nodes[node].symbol], path);
        return;
    }
    // Caso recursivo: descender por las ramas izquierda y derecha
    path[depth] = '0';
    make_codes(tree, tree->nodes[node].left, codes, path, depth + 1);
    path[depth] = '1';
    make_codes(tree, tree->nodes[node].right, codes, path, depth + 1);
}

// Lee un archivo y recopila:
// Frecuencias de cada byte
// Tamaño total del archivo
// Firma MD5
static int scan_file(const char *path, uint64_t frequencies[256], uint64_t *size, unsigned char digest[16]) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    memset(frequencies, 0, 256 * sizeof(*frequencies));
    *size = 0;
    Md5 md5;
    md5_init(&md5);
    unsigned char buffer[8192];
    size_t length;
    // Leer en bloques y actualizar frecuencias, tamaño y MD5
    while ((length = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        for (size_t i = 0; i < length; ++i) ++frequencies[buffer[i]];
        *size += length;
        md5_update(&md5, buffer, length);
    }
    int result = ferror(file) ? -1 : 0;
    fclose(file);
    if (result == 0) md5_final(&md5, digest);
    return result;
}

// Codifica datos usando el árbol de Huffman: reemplaza cada símbolo por su código binario
static int encode_file(FILE *archive, const char *path, char codes[256][256], uint64_t bit_count) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    unsigned char out = 0;   // Acumulador de bits
    int bits = 0;            // Cantidad de bits en el acumulador
    unsigned char input[8192];
    size_t length;
    while ((length = fread(input, 1, sizeof(input), file)) != 0) {
        for (size_t i = 0; i < length; ++i) {
            // Procesar cada bit del código Huffman del símbolo
            for (const char *code = codes[input[i]]; *code != '\0'; ++code) {
                out = (unsigned char)((out << 1) | (*code == '1'));
                if (++bits == 8) {
                    // Escribir un byte completo cuando acumulamos 8 bits
                    if (write_bytes(archive, &out, 1) != 0) { fclose(file); return -1; }
                    out = 0;
                    bits = 0;
                }
            }
        }
    }
    // Rellenar con ceros a la izquierda
    if (ferror(file) || (bits > 0 && write_bytes(archive, &(unsigned char){(unsigned char)(out << (8 - bits))}, 1) != 0)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    (void)bit_count;
    return 0;
}

// Comprime recursivamente todos los archivos de un directorio en un archivo HUF
static int compress_directory(const char *directory, const char *archive_path) {
    struct stat info;
    if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) {
        fprintf(stderr, "No es un directorio: %s\n", directory);
        return 1;
    }
    FileList list = {0};
    if (collect_files(directory, "", &list) != 0) { free_file_list(&list); return 1; }
    if (list.count > UINT32_MAX) { free_file_list(&list); return 1; }
    FILE *archive = fopen(archive_path, "wb");
    if (archive == NULL) {
        fprintf(stderr, "No se puede crear %s: %s\n", archive_path, strerror(errno));
        free_file_list(&list);
        return 1;
    }
    int result = 0;
    // Escribir encabezado: firma mágica y cantidad de archivos
    if (write_bytes(archive, MAGIC, 4) != 0 || write_u32(archive, (uint32_t)list.count) != 0) result = -1;
    for (size_t i = 0; i < list.count && result == 0; ++i) {
        char source[4096];
        int length = snprintf(source, sizeof(source), "%s/%s", directory, list.items[i].path);
        uint64_t frequencies[256], size, bit_count = 0;
        unsigned char digest[16];
        HuffmanTree tree;
        char codes[256][256] = {{0}}, path[256];
        if (length < 0 || (size_t)length >= sizeof(source) || scan_file(source, frequencies, &size, digest) != 0) {
            result = -1;
            break;
        }
        tree_build(&tree, frequencies);
        if (tree.root >= 0) make_codes(&tree, tree.root, codes, path, 0);
        for (int symbol = 0; symbol < 256; ++symbol) bit_count += frequencies[symbol] * strlen(codes[symbol]);
        uint16_t symbol_count = 0;
        for (int symbol = 0; symbol < 256; ++symbol) if (frequencies[symbol] != 0) ++symbol_count;
        size_t path_length = strlen(list.items[i].path);
        if (path_length > MAX_PATH_LEN || write_u16(archive, (uint16_t)path_length) != 0 ||
            write_bytes(archive, list.items[i].path, path_length) != 0 || write_u64(archive, size) != 0 ||
            write_bytes(archive, digest, 16) != 0 || write_u16(archive, symbol_count) != 0) {
            result = -1;
            break;
        }
        for (int symbol = 0; symbol < 256; ++symbol) {
            if (frequencies[symbol] == 0) continue;
            if (write_bytes(archive, &(unsigned char){(unsigned char)symbol}, 1) != 0 || write_u64(archive, frequencies[symbol]) != 0) { result = -1; break; }
        }
        if (result != 0 || write_u64(archive, bit_count) != 0 || encode_file(archive, source, codes, bit_count) != 0) result = -1;
    }
    if (fclose(archive) != 0) result = -1;
    free_file_list(&list);
    if (result != 0) {
        fprintf(stderr, "Error al comprimir.\n");
        unlink(archive_path);
        return 1;
    }
    printf("Comprimidos %zu archivos en %s\n", list.count, archive_path);
    return 0;
}

// Verifica que una ruta no intente escapar del directorio de extracción
// Rechaza rutas absolutas y referencias a directorios padre (..)
static int safe_relative_path(const char *path) {
    if (path[0] == '/' || strstr(path, "\\") != NULL) return 0;
    const char *part = path;
    while (*part != '\0') {
        const char *end = strchr(part, '/');
        size_t length = end == NULL ? strlen(part) : (size_t)(end - part);
        if (length == 0 || (length == 2 && strncmp(part, "..", 2) == 0)) return 0;  // Rechazar
        part = end == NULL ? part + length : end + 1;
    }
    return 1;
}

// Crea recursivamente todos los directorios necesarios para una ruta de archivo
// Si un directorio ya existe, no falla
static int make_parent_directories(const char *path) {
    char copy[4096];
    if (strlen(path) >= sizeof(copy)) return -1;
    strcpy(copy, path);
    for (char *slash = strchr(copy + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) return -1;
        *slash = '/';
    }
    return 0;
}

// Descodifica datos comprimidos usando el árbol de Huffman
// Lee bits secuencialmente, siguiendo el árbol hasta encontrar hojas
static int decode_file(FILE *archive, const HuffmanTree *tree, uint64_t bit_count, uint64_t size, FILE *output) {
    unsigned char input;
    int node = tree->root;
    uint64_t produced = 0;
    // Archivo vacío
    if (tree->root < 0) return size == 0 && bit_count == 0 ? 0 : -1;
    // Archivo con un único símbolo distinto
    if (tree->nodes[tree->root].symbol >= 0) {
        unsigned char byte = (unsigned char)tree->nodes[tree->root].symbol;
        for (uint64_t i = 0; i < size; ++i) if (fwrite(&byte, 1, 1, output) != 1) return -1;
        for (uint64_t i = 0; i < bit_count; ++i) {
            if (i % 8 == 0 && read_bytes(archive, &input, 1) != 0) return -1;
        }
        return bit_count == size ? 0 : -1;
    }
    // Descodificación normal: leer bits y descender por el árbol
    for (uint64_t bit = 0; bit < bit_count; ++bit) {
        if (bit % 8 == 0 && read_bytes(archive, &input, 1) != 0) return -1;
        // Extraer el bit en la posición actual
        int value = (input >> (7 - (bit % 8))) & 1;
        // Descender por la rama correspondiente
        node = value ? tree->nodes[node].right : tree->nodes[node].left;
        if (node < 0 || node >= tree->count) return -1;
        // Si llegamos a una hoja, escribir el símbolo y volver a la raíz
        if (tree->nodes[node].symbol >= 0) {
            unsigned char byte = (unsigned char)tree->nodes[node].symbol;
            if (fwrite(&byte, 1, 1, output) != 1) return -1;
            ++produced;
            node = tree->root;
        }
    }
    return produced == size && node == tree->root ? 0 : -1;
}

// Descomprime un archivo HUF y verifica cada archivo mediante MD5
// Si la verificación falla, elimina el archivo corrupto y reporta error
static int extract_archive(const char *archive_path, const char *directory) {
    FILE *archive = fopen(archive_path, "rb");
    if (archive == NULL) { fprintf(stderr, "No se puede abrir %s: %s\n", archive_path, strerror(errno)); return 1; }
    // Validar encabezado del archivo
    char magic[4];
    uint32_t file_count;
    int result = read_bytes(archive, magic, 4) != 0 || memcmp(magic, MAGIC, 4) != 0 || read_u32(archive, &file_count) != 0;
    if (result != 0) { fprintf(stderr, "Archivo HUF inválido.\n"); fclose(archive); return 1; }
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) { fclose(archive); return 1; }
    // Procesar cada archivo almacenado
    for (uint32_t file_number = 0; file_number < file_count && result == 0; ++file_number) {
        uint16_t path_length, symbol_count;
        uint64_t size, bit_count, frequencies[256] = {0};
        unsigned char expected[16];
        char path[4096];
        if (read_u16(archive, &path_length) != 0 || path_length == 0 || path_length >= sizeof(path) ||
            read_bytes(archive, path, path_length) != 0) { result = -1; break; }
        path[path_length] = '\0';
        if (!safe_relative_path(path) || read_u64(archive, &size) != 0 || read_bytes(archive, expected, 16) != 0 ||
            read_u16(archive, &symbol_count) != 0 || symbol_count > 256) { result = -1; break; }
        for (uint16_t i = 0; i < symbol_count; ++i) {
            unsigned char symbol;
            if (read_bytes(archive, &symbol, 1) != 0 || read_u64(archive, &frequencies[symbol]) != 0 || frequencies[symbol] == 0) { result = -1; break; }
        }
        if (result != 0 || read_u64(archive, &bit_count) != 0) { result = -1; break; }
        HuffmanTree tree;
        tree_build(&tree, frequencies);
        char output_path[4096];
        int length = snprintf(output_path, sizeof(output_path), "%s/%s", directory, path);
        if (length < 0 || (size_t)length >= sizeof(output_path) || make_parent_directories(output_path) != 0) { result = -1; break; }
        FILE *output = fopen(output_path, "wb");
        if (output == NULL) {
            result = -1;
            break;
        }
        int decode_result = decode_file(archive, &tree, bit_count, size, output);
        int close_result = fclose(output);
        if (decode_result != 0 || close_result != 0) {
            unlink(output_path);
            result = -1;
            break;
        }
        // Verificar que el archivo descomprimido coincide con el MD5 original
        FILE *check = fopen(output_path, "rb");
        Md5 md5;
        md5_init(&md5);
        unsigned char buffer[8192];
        size_t read_count;
        while (check != NULL && (read_count = fread(buffer, 1, sizeof(buffer), check)) != 0) md5_update(&md5, buffer, read_count);
        unsigned char actual[16];
        if (check == NULL || ferror(check)) result = -1;
        else { 
            md5_final(&md5, actual); 
            if (memcmp(expected, actual, 16) != 0) result = -2;  // MD5 no coincide: archivo corrupto
        }
        if (check != NULL) fclose(check);
        if (result != 0) { unlink(output_path); break; }  // Eliminar archivo corrupto
        printf("Verificado: %s\n", path);
    }
    fclose(archive);
    if (result != 0) { fprintf(stderr, result == -2 ? "Error MD5: archivo corrupto (%s).\n" : "Archivo HUF corrupto.\n", archive_path); return 1; }
    printf("Extraídos y verificados %u archivos en %s\n", file_count, directory);
    return 0;
}

static uint32_t left_rotate(uint32_t value, uint32_t amount) { return (value << amount) | (value >> (32 - amount)); }

static void md5_transform(Md5 *md5, const unsigned char block[64]) {
    static const uint32_t shifts[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static const uint32_t constants[64] = {0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    uint32_t words[16], a=md5->state[0], b=md5->state[1], c=md5->state[2], d=md5->state[3];
    for (int i=0;i<16;++i) words[i]=(uint32_t)block[i*4]|((uint32_t)block[i*4+1]<<8)|((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);
    for (int i=0;i<64;++i) { uint32_t f,g; if(i<16){f=(b&c)|(~b&d);g=i;} else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)%16;} else if(i<48){f=b^c^d;g=(3*i+5)%16;} else {f=c^(b|~d);g=(7*i)%16;} uint32_t temp=d; d=c; c=b; b=b+left_rotate(a+f+constants[i]+words[g],shifts[i]); a=temp; }
    md5->state[0]+=a; md5->state[1]+=b; md5->state[2]+=c; md5->state[3]+=d;
}

// Inicializar estado MD5 con valores estándar RFC 1321
static void md5_init(Md5 *md5) { md5->state[0]=0x67452301; md5->state[1]=0xefcdab89; md5->state[2]=0x98badcfe; md5->state[3]=0x10325476; md5->bit_count=0; md5->buffer_used=0; }
static void md5_update(Md5 *md5, const unsigned char *data, size_t length) { md5->bit_count += (uint64_t)length*8; while(length){ size_t copy=64-md5->buffer_used; if(copy>length)copy=length; memcpy(md5->buffer+md5->buffer_used,data,copy); md5->buffer_used+=copy; data+=copy; length-=copy; if(md5->buffer_used==64){md5_transform(md5,md5->buffer);md5->buffer_used=0;} } }
static void md5_final(Md5 *md5, unsigned char digest[16]) { uint64_t bits=md5->bit_count; unsigned char pad[64]={0x80}; md5_update(md5,pad,md5->buffer_used<56?56-md5->buffer_used:120-md5->buffer_used); unsigned char length[8]; for(int i=0;i<8;++i)length[i]=(unsigned char)(bits>>(8*i)); md5_update(md5,length,8); for(int i=0;i<4;++i) for(int j=0;j<4;++j) digest[i*4+j]=(unsigned char)(md5->state[i]>>(8*j)); }

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "compress") != 0 && strcmp(argv[1], "extract") != 0)) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "compress") == 0) return compress_directory(argv[2], argv[3]);
    return extract_archive(argv[2], argv[3]);
}