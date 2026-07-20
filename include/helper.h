#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#define RAW_DATA_ROOT "/home/scexao/VibrationControlSystem/data/raw"

static inline double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

static inline double per_million(uint64_t count, uint64_t callbacks)
{
    if (callbacks == 0) {
        return 0.0;
    }

    return 1.0e6 * (double)count / (double)callbacks;
}

static inline double rate_per_second(uint64_t count, double durationSeconds)
{
    if (durationSeconds <= 0.0) {
        return 0.0;
    }

    return (double)count / durationSeconds;
}

static inline int MKDIR_P(const char *path)
{
    char temp[PATH_MAX];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(temp, path, length + 1);

    if (temp[length - 1] == '/') {
        temp[length - 1] = '\0';
    }

    for (char *p = temp + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';

        if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
            return -1;
        }

        *p = '/';
    }

    if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static inline char *GET_OUTPUT_DIRECTORY(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <path-relative-to-data/raw>\n"
                "Example: %s 20260715/RT_Kernel/test1\n",
                argv[0], argv[0]);

        return NULL;
    }

    const char *relativePath = argv[1];
    size_t relativeLength = strlen(relativePath);

    if (relativeLength == 0) {
        fprintf(stderr, "Output path cannot be empty.\n");
        return NULL;
    }

    if (relativePath[0] == '/') {
        fprintf(stderr,
                "Output path must be relative to %s.\n",
                RAW_DATA_ROOT);

        return NULL;
    }

    /*
     * Prevent the command-line argument from escaping RAW_DATA_ROOT
     * through path components such as "../".
     */
    if (strcmp(relativePath, "..") == 0 ||
        strncmp(relativePath, "../", 3) == 0 ||
        strstr(relativePath, "/../") != NULL ||
        (relativeLength >= 3 &&
         strcmp(relativePath + relativeLength - 3, "/..") == 0)) {

        fprintf(stderr, "Parent-directory components are not allowed.\n");
        return NULL;
    }

    int requiredLength = snprintf(NULL, 0, "%s/%s",
                                  RAW_DATA_ROOT, relativePath);

    if (requiredLength < 0) {
        fprintf(stderr, "Failed to calculate output-path length.\n");
        return NULL;
    }

    char *outputDirectory = malloc((size_t)requiredLength + 1);

    if (outputDirectory == NULL) {
        perror("malloc output directory");
        return NULL;
    }

    snprintf(outputDirectory, (size_t)requiredLength + 1,
             "%s/%s", RAW_DATA_ROOT, relativePath);

    if (MKDIR_P(outputDirectory) != 0) {
        perror(outputDirectory);
        free(outputDirectory);
        return NULL;
    }

    return outputDirectory;
}

static inline int BUILD_FILE_PATH(char *destination, size_t destinationSize,
                           const char *directory, const char *filename)
{
    int written = snprintf(destination, destinationSize,
                           "%s/%s", directory, filename);

    if (written < 0 || (size_t)written >= destinationSize) {
        fprintf(stderr, "Output file path is too long: %s/%s\n",
                directory, filename);

        return -1;
    }

    return 0;
}

#endif // HELPER_H