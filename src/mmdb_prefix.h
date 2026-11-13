#ifndef KNURL_MMDB_PREFIX_H
#define KNURL_MMDB_PREFIX_H
#include "mmdb_cache.h"
#include <knurl/mmdb.h>
struct knurl_mmdb_prefix_node;
struct knurl_mmdb_prefix {
    struct knurl_mmdb_cache *heap;
    size_t limit, used;
    struct knurl_mmdb_prefix_node *roots[2], *head, *tail, *spare, *allocated;
};
int knurl_mmdb_prefix_get(struct knurl_mmdb_prefix *,
                          const knurl_mmdb_address *, knurl_mmdb_country *,
                          knurl_status *);
void knurl_mmdb_prefix_put(struct knurl_mmdb_prefix *,
                           const knurl_mmdb_address *, unsigned int,
                           const knurl_mmdb_country *, knurl_status);
void knurl_mmdb_prefix_clear(struct knurl_mmdb_prefix *);
#endif
