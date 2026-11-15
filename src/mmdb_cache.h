#ifndef KNURL_MMDB_CACHE_H
#define KNURL_MMDB_CACHE_H
#include <knurl/common.h>
#define KNURL_MMDB_PAGE_SIZE 4096u
#define KNURL_MMDB_BUCKETS 256u
struct knurl_mmdb_page;
struct knurl_mmdb_cache {
    int fd;
    size_t size, heap_limit, heap_used, block_limit, block_used;
    struct knurl_mmdb_page *head, *tail;
    struct knurl_mmdb_page *buckets[KNURL_MMDB_BUCKETS];
    knurl_status failure;
    int native_error;
};
int knurl_mmdb_cache_read(struct knurl_mmdb_cache *, size_t, void *, size_t);
/* Borrow one in-page view only until the next cache access. NULL also denotes
 * a page boundary; use cache_read in that case. */
const unsigned char *knurl_mmdb_cache_view(struct knurl_mmdb_cache *, size_t,
                                           size_t);
void knurl_mmdb_cache_clear(struct knurl_mmdb_cache *);
size_t knurl_mmdb_cache_page_size(void);
#endif
