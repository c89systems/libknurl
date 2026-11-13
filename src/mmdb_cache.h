#ifndef KNURL_MMDB_CACHE_H
#define KNURL_MMDB_CACHE_H
#include <knurl/common.h>
#define KNURL_MMDB_PAGE_SIZE 4096u
#define KNURL_MMDB_BUCKETS 256u
struct knurl_mmdb_page;
struct knurl_mmdb_cache {
    int fd;
    size_t size, heap_limit, heap_used;
    struct knurl_mmdb_page *head, *tail;
    struct knurl_mmdb_page *buckets[KNURL_MMDB_BUCKETS];
    knurl_status failure;
    int native_error;
};
int knurl_mmdb_cache_read(struct knurl_mmdb_cache *, size_t, void *, size_t);
void knurl_mmdb_cache_clear(struct knurl_mmdb_cache *);
#endif
