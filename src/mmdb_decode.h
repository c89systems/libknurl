#ifndef KNURL_MMDB_DECODE_H
#define KNURL_MMDB_DECODE_H
#include <stddef.h>
#include "mmdb_cache.h"
struct knurl_mmdb_section {
    struct knurl_mmdb_cache *cache;
    size_t base, size;
};
struct knurl_mmdb_value {
    unsigned int type;
    unsigned long size;
    size_t payload, next;
};
int knurl_mmdb_decode(const struct knurl_mmdb_section *, size_t,
                      struct knurl_mmdb_value *, unsigned long *, int);
int knurl_mmdb_skip(const struct knurl_mmdb_section *, size_t *,
                    unsigned long *);
int knurl_mmdb_select(const struct knurl_mmdb_section *,
                      const struct knurl_mmdb_value *, const char *,
                      struct knurl_mmdb_value *, unsigned long *);
unsigned long knurl_mmdb_integer(const struct knurl_mmdb_section *,
                                 const struct knurl_mmdb_value *);
int knurl_mmdb_read(const struct knurl_mmdb_section *, size_t, void *, size_t);
int knurl_mmdb_equal(const struct knurl_mmdb_section *, size_t, const void *,
                     size_t);
int knurl_mmdb_same(const struct knurl_mmdb_section *, size_t, size_t, size_t);
#endif
