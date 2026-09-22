#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *name;
    const char *executable;
    const char *slug;
    double compression_seconds;
    double extraction_seconds;
    uint32_t verified_files;
    uint64_t compressed_bytes;
} ProgramResult;

static const char *PROGRAM_NAMES[] = {
    "Serial",
    "Paralela",
    "Concurrente"
};

static const char *PROGRAM_EXECUTABLES[] = {
    "compresor",
    "compresor_paralelo",
    "compresor_concurrente"
};

static const char *PROGRAM_SLUGS[] = {
    "serial",
    "paralela",
    "concurrente"
};

static const size_t PROGRAM_COUNT = 3;

static void usage(const char *program)
{
    fprintf(
        stderr,
        "Uso:\n"
        "  %s compress <directorio_entrada> <directorio_resultados>\n"
        "  %s extract  <archivo.huf> <directorio_resultados>\n"
        "  %s compare  <directorio_entrada> <directorio_resultados>\n",
        program,
        program,
        program
    );
}

static double elapsed_seconds(
    const struct timespec *start,
    const struct timespec *end
) {
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static int join_path(
    char destination[PATH_MAX],
    const char *left,
    const char *right
) {
    int written = snprintf(destination, PATH_MAX, "%s/%s", left, right);
    return written >= 0 && written < PATH_MAX ? 0 : -1;
}

static int get_program_directory(
    const char *argv0,
    char directory[PATH_MAX]
) {
    char resolved[PATH_MAX];

    if (realpath(argv0, resolved) == NULL) {
        return -1;
    }

    char *slash = strrchr(resolved, '/');
    if (slash == NULL) {
        return -1;
    }

    *slash = '\0';

    if (strlen(resolved) >= PATH_MAX) {
        return -1;
    }

    strcpy(directory, resolved);
    return 0;
}

static int make_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        struct stat info;
        return stat(path, &info) == 0 && S_ISDIR(info.st_mode) ? 0 : -1;
    }

    return -1;
}

static int create_run_directory(
    const char *base_directory,
    char run_directory[PATH_MAX]
) {
    if (make_directory(base_directory) != 0) {
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }

    struct tm local_time;
    if (localtime_r(&now.tv_sec, &local_time) == NULL) {
        return -1;
    }

    char run_name[128];
    int written = snprintf(
        run_name,
        sizeof(run_name),
        "ejecucion_%04d%02d%02d_%02d%02d%02d_%ld",
        local_time.tm_year + 1900,
        local_time.tm_mon + 1,
        local_time.tm_mday,
        local_time.tm_hour,
        local_time.tm_min,
        local_time.tm_sec,
        (long)getpid()
    );

    if (written < 0 || (size_t)written >= sizeof(run_name) ||
        join_path(run_directory, base_directory, run_name) != 0) {
        return -1;
    }

    return mkdir(run_directory, 0755) == 0 ? 0 : -1;
}

static int directory_stats_recursive(
    const char *directory,
    uint32_t *file_count,
    uint64_t *total_bytes
) {
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return -1;
    }

    int result = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && result == 0) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PATH_MAX];
        if (join_path(path, directory, entry->d_name) != 0) {
            result = -1;
            break;
        }

        struct stat info;
        if (lstat(path, &info) != 0) {
            result = -1;
        } else if (S_ISDIR(info.st_mode)) {
            result = directory_stats_recursive(path, file_count, total_bytes);
        } else if (S_ISREG(info.st_mode)) {
            if (*file_count == UINT32_MAX ||
                UINT64_MAX - *total_bytes < (uint64_t)info.st_size) {
                result = -1;
            } else {
                ++(*file_count);
                *total_bytes += (uint64_t)info.st_size;
            }
        }
    }

    if (closedir(dir) != 0) {
        result = -1;
    }

    return result;
}

static int directory_stats(
    const char *directory,
    uint32_t *file_count,
    uint64_t *total_bytes
) {
    *file_count = 0;
    *total_bytes = 0;
    return directory_stats_recursive(directory, file_count, total_bytes);
}

static int regular_file_size(const char *path, uint64_t *size)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return -1;
    }

    *size = (uint64_t)info.st_size;
    return 0;
}

static int run_program(
    const char *executable,
    const char *operation,
    const char *input,
    const char *output,
    double *seconds,
    uint32_t *verified_files
) {
    int output_pipe[2];
    if (pipe(output_pipe) != 0) {
        return -1;
    }

    struct timespec start;
    struct timespec end;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }

    pid_t child = fork();

    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }

    if (child == 0) {
        close(output_pipe[0]);

        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(output_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }

        close(output_pipe[1]);
        execl(executable, executable, operation, input, output, (char *)NULL);
        _exit(127);
    }

    close(output_pipe[1]);
    FILE *stream = fdopen(output_pipe[0], "r");

    if (stream == NULL) {
        close(output_pipe[0]);
        waitpid(child, NULL, 0);
        return -1;
    }

    *verified_files = 0;
    char *line = NULL;
    size_t capacity = 0;

    while (getline(&line, &capacity, stream) >= 0) {
        if (strncmp(line, "Verificado:", 11) == 0) {
            ++(*verified_files);
        }
        fputs(line, stderr);
    }

    free(line);
    int read_failed = ferror(stream);
    fclose(stream);

    int child_status;
    int wait_result = waitpid(child, &child_status, 0);

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return -1;
    }

    *seconds = elapsed_seconds(&start, &end);

    if (read_failed || wait_result < 0 ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return -1;
    }

    return 0;
}

static double acceleration(double serial_seconds, double compared_seconds)
{
    if (serial_seconds <= 0.0) {
        return 0.0;
    }

    return (serial_seconds - compared_seconds) / serial_seconds * 100.0;
}

static void print_result(
    const ProgramResult *result,
    uint32_t total_files,
    uint64_t original_bytes,
    double compression_acceleration,
    double extraction_acceleration
) {
    double health;
    if (result->extraction_seconds < 0.0) {
        health = -1.0;
    } else if (total_files == 0) {
        health = 100.0;
    } else {
        health = (double)result->verified_files /
                 (double)total_files * 100.0;
    }

    double ratio = original_bytes == 0
        ? 0.0
        : (double)result->compressed_bytes / (double)original_bytes * 100.0;

    printf(
        "RESULT=%s|%.2f|%.6f|%.6f|%.2f|%.2f|%u|%u|%llu|%llu|%.2f\n",
        result->name,
        health,
        result->compression_seconds,
        result->extraction_seconds,
        compression_acceleration,
        extraction_acceleration,
        total_files,
        result->verified_files,
        (unsigned long long)original_bytes,
        (unsigned long long)result->compressed_bytes,
        ratio
    );
}

static int prepare_results(ProgramResult results[3])
{
    for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
        results[i] = (ProgramResult){
            .name = PROGRAM_NAMES[i],
            .executable = PROGRAM_EXECUTABLES[i],
            .slug = PROGRAM_SLUGS[i],
            .compression_seconds = -1.0,
            .extraction_seconds = -1.0,
            .verified_files = 0,
            .compressed_bytes = 0
        };
    }

    return 0;
}

static int build_executable_paths(
    ProgramResult results[3],
    const char *program_directory,
    char paths[3][PATH_MAX]
) {
    for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
        if (join_path(paths[i], program_directory, results[i].executable) != 0 ||
            access(paths[i], X_OK) != 0) {
            fprintf(stderr, "No se puede ejecutar %s. Ejecute make primero.\n", results[i].executable);
            return -1;
        }
        results[i].executable = paths[i];
    }

    return 0;
}

static int run_compression(
    ProgramResult results[3],
    const char *input_directory,
    const char *run_directory,
    char archives[3][PATH_MAX]
) {
    for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
        char archive_name[128];
        int written = snprintf(
            archive_name,
            sizeof(archive_name),
            "%s.huf",
            results[i].slug
        );

        if (written < 0 || (size_t)written >= sizeof(archive_name) ||
            join_path(archives[i], run_directory, archive_name) != 0) {
            return -1;
        }

        uint32_t ignored_verified;
        fprintf(stderr, "Ejecutando compresion %s...\n", results[i].name);

        if (run_program(
                results[i].executable,
                "compress",
                input_directory,
                archives[i],
                &results[i].compression_seconds,
                &ignored_verified
            ) != 0 ||
            regular_file_size(archives[i], &results[i].compressed_bytes) != 0) {
            fprintf(stderr, "Fallo la compresion %s.\n", results[i].name);
            return -1;
        }
    }

    return 0;
}

static int run_extraction(
    ProgramResult results[3],
    char archives[3][PATH_MAX],
    const char *single_archive,
    const char *run_directory
) {
    for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
        char output_name[128];
        int written = snprintf(
            output_name,
            sizeof(output_name),
            "extraido_%s",
            results[i].slug
        );

        char output_directory[PATH_MAX];
        if (written < 0 || (size_t)written >= sizeof(output_name) ||
            join_path(output_directory, run_directory, output_name) != 0) {
            return -1;
        }

        const char *archive = single_archive == NULL ? archives[i] : single_archive;
        fprintf(stderr, "Ejecutando descompresion %s...\n", results[i].name);

        if (run_program(
                results[i].executable,
                "extract",
                archive,
                output_directory,
                &results[i].extraction_seconds,
                &results[i].verified_files
            ) != 0) {
            fprintf(stderr, "Fallo la descompresion %s.\n", results[i].name);
            return -1;
        }

        if (single_archive != NULL &&
            regular_file_size(single_archive, &results[i].compressed_bytes) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4 ||
        (strcmp(argv[1], "compress") != 0 &&
         strcmp(argv[1], "extract") != 0 &&
         strcmp(argv[1], "compare") != 0)) {
        usage(argv[0]);
        return 2;
    }

    char program_directory[PATH_MAX];
    if (get_program_directory(argv[0], program_directory) != 0) {
        fprintf(stderr, "No se pudo determinar la carpeta del comparador.\n");
        return 1;
    }

    char run_directory[PATH_MAX];
    if (create_run_directory(argv[3], run_directory) != 0) {
        fprintf(stderr, "No se pudo crear la carpeta de ejecucion.\n");
        return 1;
    }

    ProgramResult results[3];
    prepare_results(results);

    char executable_paths[3][PATH_MAX];
    if (build_executable_paths(results, program_directory, executable_paths) != 0) {
        return 1;
    }

    char archives[3][PATH_MAX] = {{0}};
    uint32_t total_files = 0;
    uint64_t original_bytes = 0;

    if (strcmp(argv[1], "compress") == 0 ||
        strcmp(argv[1], "compare") == 0) {
        if (directory_stats(argv[2], &total_files, &original_bytes) != 0) {
            fprintf(stderr, "No se pudo analizar el directorio de entrada.\n");
            return 1;
        }

        if (run_compression(results, argv[2], run_directory, archives) != 0) {
            return 1;
        }
    }

    if (strcmp(argv[1], "extract") == 0) {
        if (run_extraction(results, archives, argv[2], run_directory) != 0) {
            return 1;
        }

        total_files = results[0].verified_files;

        char serial_output[PATH_MAX];
        if (join_path(serial_output, run_directory, "extraido_serial") != 0 ||
            directory_stats(serial_output, &total_files, &original_bytes) != 0) {
            return 1;
        }
    } else if (strcmp(argv[1], "compare") == 0) {
        if (run_extraction(results, archives, NULL, run_directory) != 0) {
            return 1;
        }
    }

    for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
        double compression_gain = results[i].compression_seconds < 0.0
            ? -1.0
            : (i == 0
                ? 0.0
                : acceleration(
                    results[0].compression_seconds,
                    results[i].compression_seconds
                ));

        double extraction_gain = results[i].extraction_seconds < 0.0
            ? -1.0
            : (i == 0
                ? 0.0
                : acceleration(
                    results[0].extraction_seconds,
                    results[i].extraction_seconds
                ));

        print_result(
            &results[i],
            total_files,
            original_bytes,
            compression_gain,
            extraction_gain
        );
    }

    printf("RUN_DIRECTORY=%s\n", run_directory);
    return 0;
}