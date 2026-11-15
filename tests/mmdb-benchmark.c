#define _POSIX_C_SOURCE 200809L
#include <knurl/mmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifdef KNURL_MMDB_BENCHMARK_PROBE
#include "mmdb-probe.h"
#endif

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
static int parse_size(const char *text, size_t *out)
{
    unsigned long value;
    char *end;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || text[0] < '0' || text[0] > '9' || *end != 0 ||
        (size_t)value != value)
        return 0;
    *out = (size_t)value;
    return 1;
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_mmdb_file_config config;
    knurl_mmdb_address *addresses;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    unsigned long state = 17, checksum = 0;
    size_t i, j, round, count = 65536, index;
    double start, opened, first = 0, seconds, samples[8], value;
    long initial_rss, open_rss;
    struct stat info;
    int hot;
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    struct knurl_mmdb_probe_stats at_open, at_first, at_end, at_close;
#endif
    if (argc < 3 || argc > 5 ||
        (strcmp(argv[2], "hot") != 0 && strcmp(argv[2], "random") != 0)) {
        fprintf(stderr, "usage: mmdb-benchmark DATABASE hot|random [HEAP_BYTES "
                        "[ADDRESS_COUNT]]\n");
        return 2;
    }
    knurl_mmdb_file_config_init(&config);
    if (argc >= 4 && !parse_size(argv[3], &config.max_heap_bytes)) {
        fprintf(stderr, "HEAP_BYTES must be an unsigned byte count\n");
        return 2;
    }
    if (argc == 5 && (!parse_size(argv[4], &count) || count == 0 ||
                      count > (size_t)-1 / sizeof(*addresses))) {
        fprintf(stderr,
                "ADDRESS_COUNT must be positive and fit the address array\n");
        return 2;
    }
    hot = strcmp(argv[2], "hot") == 0;
    initial_rss = rss();
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    knurl_mmdb_probe_start();
#endif
    start = now();
    status = knurl_mmdb_open(argv[1], &config, &db, &error);
    opened = now() - start;
    if (status != KNURL_OK) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    knurl_mmdb_probe_stat(&at_open);
    knurl_mmdb_probe_enable(0);
#endif
    open_rss = rss();
    if (stat(argv[1], &info) != 0)
        abort();
    addresses = (knurl_mmdb_address *)calloc(count, sizeof(*addresses));
    if (addresses == NULL)
        abort();
    for (i = 0; i < count; ++i) {
        addresses[i].size = i % 2 ? 4 : 16;
        for (j = 0; j < addresses[i].size; ++j)
            addresses[i].bytes[j] = (unsigned char)(next(&state) >> 24);
    }
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    knurl_mmdb_probe_enable(1);
#endif
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
        if (round == 0) {
            first = seconds;
#ifdef KNURL_MMDB_BENCHMARK_PROBE
            knurl_mmdb_probe_stat(&at_first);
#endif
        } else
            samples[round - 1] = seconds;
        if (round == 8) {
            for (i = 1; i < 8; ++i) {
                value = samples[i];
                for (j = i; j > 0 && samples[j - 1] > value; --j)
                    samples[j] = samples[j - 1];
                samples[j] = value;
            }
#ifdef KNURL_MMDB_BENCHMARK_PROBE
            knurl_mmdb_probe_stat(&at_end);
            knurl_mmdb_probe_enable(0);
#endif
            printf("heap=%lu %s address_count=%lu open_us=%.1f first_ns=%.1f "
                   "warm_ns=%.1f "
                   "checksum=%lu "
                   "database_bytes=%.0f initial_rss_kib=%ld open_rss_kib=%ld "
                   "final_rss_kib=%ld corpus_bytes=%lu\n",
                   (unsigned long)(config.max_heap_bytes
                                       ? config.max_heap_bytes
                                       : KNURL_MMDB_HEAP_DEFAULT),
                   argv[2], (unsigned long)count, opened * 1e6,
                   first * 1e9 / count,
                   (samples[3] + samples[4]) * 0.5e9 / count, checksum,
                   (double)info.st_size, initial_rss, open_rss, rss(),
                   (unsigned long)(count * sizeof(*addresses)));
        }
    }
    free(addresses);
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    knurl_mmdb_probe_enable(1);
#endif
    db->close(db, NULL);
#ifdef KNURL_MMDB_BENCHMARK_PROBE
    knurl_mmdb_probe_stat(&at_close);
    knurl_mmdb_probe_enable(0);
    if (at_close.live != 0 ||
        (at_end.prefix_hits - at_first.prefix_hits) +
                (at_end.prefix_misses - at_first.prefix_misses) !=
            8 * count ||
        at_end.peak > (config.max_heap_bytes ? config.max_heap_bytes
                                             : KNURL_MMDB_HEAP_DEFAULT))
        abort();
    printf("instrumented=1 opened_heap=%lu reader_live_heap=%lu "
           "reader_peak_heap=%lu "
           "first_preads=%lu warm_preads=%lu warm_preads_per_lookup=%.6f "
           "retained_after_close=%lu warm_prefix_hits=%lu "
           "warm_prefix_misses=%lu\n",
           (unsigned long)at_open.live, (unsigned long)at_end.live,
           (unsigned long)at_end.peak,
           (unsigned long)(at_first.reads - at_open.reads),
           (unsigned long)(at_end.reads - at_first.reads),
           (double)(at_end.reads - at_first.reads) / (8.0 * count),
           (unsigned long)at_close.live,
           (unsigned long)(at_end.prefix_hits - at_first.prefix_hits),
           (unsigned long)(at_end.prefix_misses - at_first.prefix_misses));
#endif
    return 0;
}
