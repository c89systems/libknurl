#define _POSIX_C_SOURCE 200809L
#include "mmdb_cache.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct knurl_mmdb_page {
    struct knurl_mmdb_page *older, *newer, *hash_next;
    size_t offset;
    int valid;
    unsigned char bytes[KNURL_MMDB_PAGE_SIZE];
};
static size_t bucket(size_t offset)
{
    return (offset / KNURL_MMDB_PAGE_SIZE) & (KNURL_MMDB_BUCKETS - 1);
}
static void newest(struct knurl_mmdb_cache *cache, struct knurl_mmdb_page *page)
{
    if (cache->head == page)
        return;
    if (page->newer != NULL)
        page->newer->older = page->older;
    if (page->older != NULL)
        page->older->newer = page->newer;
    if (cache->tail == page)
        cache->tail = page->newer;
    page->newer = NULL;
    page->older = cache->head;
    if (cache->head != NULL)
        cache->head->newer = page;
    else
        cache->tail = page;
    cache->head = page;
}
static void unhash(struct knurl_mmdb_cache *cache, struct knurl_mmdb_page *page)
{
    struct knurl_mmdb_page **at;
    if (!page->valid)
        return;
    at = &cache->buckets[bucket(page->offset)];
    while (*at != page)
        at = &(*at)->hash_next;
    *at = page->hash_next;
    page->valid = 0;
}
static struct knurl_mmdb_page *load(struct knurl_mmdb_cache *cache,
                                    size_t offset)
{
    struct knurl_mmdb_page *page = NULL;
    size_t expected, done = 0, slot = bucket(offset);
    ssize_t got;
    if (cache->head != NULL && cache->head->valid &&
        cache->head->offset == offset)
        return cache->head;
    for (page = cache->buckets[slot]; page != NULL; page = page->hash_next)
        if (page->offset == offset) {
            newest(cache, page);
            return page;
        }
    if (sizeof(*page) <= cache->heap_limit - cache->heap_used) {
        page = (struct knurl_mmdb_page *)malloc(sizeof(*page));
        if (page != NULL) {
            page->older = NULL;
            page->newer = NULL;
            page->valid = 0;
            cache->heap_used += sizeof(*page);
        }
    }
    if (page == NULL) {
        page = cache->tail;
        if (page == NULL) {
            cache->failure = KNURL_NOMEM;
            return NULL;
        }
        unhash(cache, page);
    }
    newest(cache, page);
    page->offset = offset;
    page->valid = 0;
    expected = cache->size - offset;
    if (expected > KNURL_MMDB_PAGE_SIZE)
        expected = KNURL_MMDB_PAGE_SIZE;
    while (done < expected) {
        got = pread(cache->fd, page->bytes + done, expected - done,
                    (off_t)(offset + done));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            cache->failure = KNURL_IO;
            cache->native_error = got < 0 ? errno : EIO;
            return NULL;
        }
        done += (size_t)got;
    }
    page->valid = 1;
    page->hash_next = cache->buckets[slot];
    cache->buckets[slot] = page;
    return page;
}
int knurl_mmdb_cache_read(struct knurl_mmdb_cache *cache, size_t offset,
                          void *destination, size_t length)
{
    struct knurl_mmdb_page *page;
    unsigned char *out = (unsigned char *)destination;
    size_t within, count;
    if (cache->failure != KNURL_OK || offset > cache->size ||
        length > cache->size - offset)
        return 0;
    while (length != 0) {
        within = offset & (KNURL_MMDB_PAGE_SIZE - 1);
        page = load(cache, offset - within);
        if (page == NULL)
            return 0;
        count = KNURL_MMDB_PAGE_SIZE - within;
        if (count > length)
            count = length;
        memcpy(out, page->bytes + within, count);
        offset += count;
        out += count;
        length -= count;
    }
    return 1;
}
void knurl_mmdb_cache_clear(struct knurl_mmdb_cache *cache)
{
    struct knurl_mmdb_page *page, *next;
    for (page = cache->head; page != NULL; page = next) {
        next = page->older;
        cache->heap_used -= sizeof(*page);
        free(page);
    }
    cache->head = NULL;
    cache->tail = NULL;
    memset(cache->buckets, 0, sizeof(cache->buckets));
}
