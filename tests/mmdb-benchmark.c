#define _POSIX_C_SOURCE 200809L
#include <knurl/mmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        abort();
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static unsigned long next(unsigned long *state)
{
    *state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
    return *state;
}
static long rss(void)
{
#ifdef __linux__
    FILE *file;
    unsigned long virtual_pages, resident_pages;
    file = fopen("/proc/self/statm", "r");
    if (file == NULL ||
        fscanf(file, "%lu %lu", &virtual_pages, &resident_pages) != 2)
        abort();
    fclose(file);
    return (long)(resident_pages * (unsigned long)sysconf(_SC_PAGESIZE) / 1024);
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        abort();
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
#endif
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_mmdb_address *addresses;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    unsigned long state = 17, checksum = 0;
    size_t i, j, round, count = 65536, index;
    double start, opened, first = 0, seconds, samples[8], value;
    long initial_rss, open_rss;
    struct stat info;
    int hot, native;
    if (argc != 4 ||
        (strcmp(argv[1], "maxmind") != 0 && strcmp(argv[1], "native") != 0) ||
        (strcmp(argv[3], "hot") != 0 && strcmp(argv[3], "random") != 0)) {
        fprintf(stderr,
                "usage: mmdb-benchmark maxmind|native DATABASE hot|random\n");
        return 2;
    }
    hot = strcmp(argv[3], "hot") == 0;
    native = strcmp(argv[1], "native") == 0;
    initial_rss = rss();
    start = now();
    status = native ? knurl_mmdb_native_open(argv[2], &db, &error)
                    : knurl_mmdb_maxmind_open(argv[2], &db, &error);
    opened = now() - start;
    if (status != KNURL_OK) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
    open_rss = rss();
    if (stat(argv[2], &info) != 0)
        abort();
    addresses = (knurl_mmdb_address *)calloc(count, sizeof(*addresses));
    if (addresses == NULL)
        abort();
    for (i = 0; i < count; ++i) {
        addresses[i].size = i % 2 ? 4 : 16;
        for (j = 0; j < addresses[i].size; ++j)
            addresses[i].bytes[j] = (unsigned char)(next(&state) >> 24);
    }
    for (round = 0; round < 9; ++round) {
        start = now();
        for (i = 0; i < count; ++i) {
            index = hot ? i % 16 : i;
            status = db->lookup(db, &addresses[index], &country, &error);
            if (status != KNURL_OK && status != KNURL_EOF) {
                fprintf(stderr, "%s\n", error.message);
                return 1;
            }
            checksum =
                (checksum * 33UL + (unsigned char)country.code[0] * 256UL +
                 (unsigned char)country.code[1] + (unsigned long)status) &
                0xffffffffUL;
        }
        seconds = now() - start;
        if (round == 0)
            first = seconds;
        else
            samples[round - 1] = seconds;
        if (round == 8) {
            for (i = 1; i < 8; ++i) {
                value = samples[i];
                for (j = i; j > 0 && samples[j - 1] > value; --j)
                    samples[j] = samples[j - 1];
                samples[j] = value;
            }
            printf("%s %s open_us=%.1f first_ns=%.1f warm_ns=%.1f checksum=%lu "
                   "mapped_bytes=%.0f initial_rss_kib=%ld open_rss_kib=%ld "
                   "final_rss_kib=%ld corpus_bytes=%lu\n",
                   argv[1], argv[3], opened * 1e6, first * 1e9 / count,
                   (samples[3] + samples[4]) * 0.5e9 / count, checksum,
                   (double)info.st_size, initial_rss, open_rss, rss(),
                   (unsigned long)(count * sizeof(*addresses)));
        }
    }
    free(addresses);
    db->close(db, NULL);
    return 0;
}
