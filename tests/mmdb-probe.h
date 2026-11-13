#ifndef KNURL_TEST_MMDB_PROBE_H
#define KNURL_TEST_MMDB_PROBE_H
#include <stddef.h>
struct knurl_mmdb_probe_stats {
    size_t live, peak, reads;
};
void knurl_mmdb_probe_start(void);
void knurl_mmdb_probe_enable(int);
void knurl_mmdb_probe_stat(struct knurl_mmdb_probe_stats *);
#endif
