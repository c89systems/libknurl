#define _POSIX_C_SOURCE 200809L
/* Track reader-owned requested heap and I/O without allocator/RSS noise. */
#include <knurl/mmdb.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef KNURL_MMDB_BENCHMARK_PROBE
#include "mmdb-probe.h"
#include "../src/mmdb_prefix.h"
int __real_knurl_mmdb_prefix_get(struct knurl_mmdb_prefix *,
                                 const knurl_mmdb_address *,
                                 knurl_mmdb_country *, knurl_status *);
int __wrap_knurl_mmdb_prefix_get(struct knurl_mmdb_prefix *,
                                 const knurl_mmdb_address *,
                                 knurl_mmdb_country *, knurl_status *);
static size_t prefix_hits, prefix_misses;
int __wrap_knurl_mmdb_prefix_get(struct knurl_mmdb_prefix *cache,
                                 const knurl_mmdb_address *address,
                                 knurl_mmdb_country *country,
                                 knurl_status *status)
{
    int hit = __real_knurl_mmdb_prefix_get(cache, address, country, status);
    if (hit)
        ++prefix_hits;
    else
        ++prefix_misses;
    return hit;
}
#endif
#ifdef KNURL_MMDB_PREAD64
#define MMDB_REAL_PREAD __real_pread64
#define MMDB_WRAP_PREAD __wrap_pread64
#define MMDB_WRAP_MMAP __wrap_mmap64
#else
#define MMDB_REAL_PREAD __real_pread
#define MMDB_WRAP_PREAD __wrap_pread
#define MMDB_WRAP_MMAP __wrap_mmap
#endif
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
ssize_t MMDB_REAL_PREAD(int, void *, size_t, off_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
ssize_t MMDB_WRAP_PREAD(int, void *, size_t, off_t);
void *MMDB_WRAP_MMAP(void *, size_t, int, int, int, off_t);
struct allocation {
    void *pointer;
    size_t size;
};
#define ALLOCATION_SLOTS 262144u
static struct allocation allocations[ALLOCATION_SLOTS];
static size_t live, peak, calls, attempts, fail_at, reads;
static int measuring, deny_all, io_mode;
static size_t allocation_hash(void *pointer)
{
    size_t value = (size_t)pointer;
    return ((value >> 4) ^ (value >> 17)) & (ALLOCATION_SLOTS - 1);
}
static size_t allocation_find(void *pointer)
{
    size_t slot, i;
    if (pointer == NULL)
        return ALLOCATION_SLOTS;
    slot = allocation_hash(pointer);
    for (i = 0; i < ALLOCATION_SLOTS; ++i) {
        if (allocations[slot].pointer == pointer)
            return slot;
        if (allocations[slot].pointer == NULL &&
            allocations[slot].size != (size_t)-1)
            return ALLOCATION_SLOTS;
        slot = (slot + 1) & (ALLOCATION_SLOTS - 1);
    }
    return ALLOCATION_SLOTS;
}
static int reject(void)
{
    if (!measuring)
        return 0;
    ++attempts;
    return deny_all || (fail_at != 0 && attempts == fail_at);
}
static void forget(void *pointer)
{
    size_t slot = allocation_find(pointer);
    if (slot < ALLOCATION_SLOTS) {
        live -= allocations[slot].size;
        allocations[slot].pointer = NULL;
        allocations[slot].size = (size_t)-1;
    }
}
static void *remember(void *pointer, size_t size)
{
    size_t i, slot;
    if (!measuring || pointer == NULL)
        return pointer;
    slot = allocation_hash(pointer);
    for (i = 0; i < ALLOCATION_SLOTS; ++i) {
        if (allocations[slot].pointer == NULL) {
            allocations[slot].pointer = pointer;
            allocations[slot].size = size;
            live += size;
            if (live > peak)
                peak = live;
            ++calls;
            return pointer;
        }
        slot = (slot + 1) & (ALLOCATION_SLOTS - 1);
    }
    abort();
    return NULL;
}
void *__wrap_malloc(size_t size)
{
    return reject() ? NULL : remember(__real_malloc(size), size);
}
void *__wrap_calloc(size_t count, size_t size)
{
    return reject() ? NULL : remember(__real_calloc(count, size), count * size);
}
void *__wrap_realloc(void *pointer, size_t size)
{
    size_t index;
    void *result;
    if (reject())
        return NULL;
    index = allocation_find(pointer);
    result = __real_realloc(pointer, size);
    if ((result != NULL || size == 0) && index < ALLOCATION_SLOTS) {
        live -= allocations[index].size;
        allocations[index].pointer = NULL;
        allocations[index].size = (size_t)-1;
    }
    return remember(result, size);
}
void __wrap_free(void *pointer)
{
    forget(pointer);
    __real_free(pointer);
}
ssize_t MMDB_WRAP_PREAD(int fd, void *out, size_t length, off_t offset)
{
    assert(length > 0 && length <= 4096);
    ++reads;
    if (io_mode == 1) {
        io_mode = 0;
        errno = EINTR;
        return -1;
    }
    if (io_mode == 3) {
        errno = EIO;
        return -1;
    }
    if (io_mode == 4)
        return 0;
    if (io_mode == 2 && length > 7)
        length = 7;
    return MMDB_REAL_PREAD(fd, out, length, offset);
}
void *MMDB_WRAP_MMAP(void *address, size_t length, int protection, int flags,
                     int fd, off_t offset)
{
    (void)address;
    (void)length;
    (void)protection;
    (void)flags;
    (void)fd;
    (void)offset;
    assert(0 && "MMDB must not use mmap");
    return MAP_FAILED;
}
#ifdef KNURL_MMDB_BENCHMARK_PROBE
void knurl_mmdb_probe_start(void)
{
    assert(live == 0);
    peak = calls = attempts = reads = fail_at = 0;
    prefix_hits = prefix_misses = 0;
    deny_all = io_mode = 0;
    measuring = 1;
}
void knurl_mmdb_probe_enable(int enabled) { measuring = enabled; }
void knurl_mmdb_probe_stat(struct knurl_mmdb_probe_stats *out)
{
    out->live = live;
    out->peak = peak;
    out->reads = reads;
    out->prefix_hits = prefix_hits;
    out->prefix_misses = prefix_misses;
}
#else
static void query(knurl_mmdb *db, unsigned int index)
{
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_error error;
    address.size = 4;
    memset(address.bytes, 0, sizeof(address.bytes));
    address.bytes[0] = 100;
    address.bytes[2] = (unsigned char)index;
    address.bytes[3] = 1;
    assert(db->lookup(db, &address, &country, &error) == KNURL_OK);
    assert(strcmp(country.code, index % 2 ? "SE" : "US") == 0);
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_mmdb_file_config config;
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    size_t before, opened, grown, open_reads, saved_peak, limit, i, fault,
        initial;
    assert(argc == 3);
    knurl_mmdb_file_config_init(&config);
    config.max_heap_bytes = (size_t)strtoul(argv[2], NULL, 10);
    limit =
        config.max_heap_bytes ? config.max_heap_bytes : KNURL_MMDB_HEAP_DEFAULT;
    measuring = 1;
    assert(knurl_mmdb_open(argv[1], &config, &db, &error) == KNURL_OK);
    opened = live;
    open_reads = reads;
    initial = attempts;
    assert(opened < limit && peak <= limit && open_reads > 0);
    if (limit >= 65536)
        assert(opened < limit / 2);
    for (i = 0; i < 128; ++i)
        query(db, (unsigned int)i);
    grown = live;
    before = calls;
    for (i = 0; i < 256; ++i)
        query(db, (unsigned int)(i % 128));
    assert(calls == before && live == grown && peak <= limit);
    if (limit >= 16384) {
        before = reads;
        for (i = 0; i < 100; ++i)
            query(db, 127);
        assert(reads == before);
        assert(grown > opened);
    }
    saved_peak = peak;
    db->close(db, NULL);
    assert(live == 0);

    /* Initial failures clean up; growth failures may reuse an existing page. */
    for (fault = 1; fault <= initial; ++fault) {
        attempts = 0;
        fail_at = fault;
        status = knurl_mmdb_open(argv[1], &config, &db, &error);
        fail_at = 0;
        if (fault == 1 || fault == 2 || fault == initial)
            assert(status == KNURL_NOMEM && db == NULL);
        if (status == KNURL_OK)
            db->close(db, NULL);
        else
            assert(status == KNURL_NOMEM && db == NULL);
        assert(live == 0);
    }
    /* Cache growth can fall back to its existing page on allocation failure. */
    assert(knurl_mmdb_open(argv[1], &config, &db, &error) == KNURL_OK);
    before = live;
    deny_all = 1;
    for (i = 0; i < 16; ++i)
        query(db, (unsigned int)i);
    assert(live == before);
    deny_all = 0;
    db->close(db, NULL);
    assert(live == 0);
    for (fault = 1; fault <= 6; ++fault) {
        assert(knurl_mmdb_open(argv[1], &config, &db, &error) == KNURL_OK);
        query(db, 0);
        attempts = 0;
        fail_at = fault;
        for (i = 1; i < 16; ++i)
            query(db, (unsigned int)i);
        fail_at = 0;
        db->close(db, NULL);
        assert(live == 0 && peak <= limit);
    }

    for (fault = 1; fault <= 4; ++fault) {
        io_mode = (int)fault;
        status = knurl_mmdb_open(argv[1], &config, &db, &error);
        if (fault <= 2) {
            assert(status == KNURL_OK);
            query(db, 0);
            db->close(db, NULL);
        } else {
            assert(status == KNURL_IO && db == NULL);
            assert(error.source == KNURL_ERROR_SYSTEM &&
                   error.native_code == EIO);
        }
        io_mode = 0;
        assert(live == 0);
    }
    assert(knurl_mmdb_open(argv[1], &config, &db, &error) == KNURL_OK);
    assert(knurl_mmdb_address_parse("100.0.64.1", &address, &error) ==
           KNURL_OK);
    io_mode = 3;
    assert(db->lookup(db, &address, &country, &error) == KNURL_IO);
    assert(country.code[0] == 0 && error.native_code == EIO);
    io_mode = 0;
    query(db, 64);
    db->close(db, NULL);
    assert(live == 0 && peak <= limit);
    /* A one-page block cache cannot retain both these records. A different
     * address in query 0's /24 must reuse the resolved country fact. */
    config.max_heap_bytes = KNURL_MMDB_HEAP_MIN;
    assert(knurl_mmdb_open(argv[1], &config, &db, &error) == KNURL_OK);
    query(db, 0);
    query(db, 127);
    before = reads;
    assert(knurl_mmdb_address_parse("100.0.0.200", &address, &error) ==
           KNURL_OK);
    io_mode = 3;
    assert(db->lookup(db, &address, &country, &error) == KNURL_OK);
    assert(strcmp(country.code, "US") == 0 && reads == before);
    io_mode = 0;
    assert(knurl_mmdb_address_parse("100.0.128.1", &address, &error) ==
           KNURL_OK);
    assert(db->lookup(db, &address, &country, &error) == KNURL_EOF);
    query(db, 127);
    before = reads;
    assert(knurl_mmdb_address_parse("100.0.200.1", &address, &error) ==
           KNURL_OK);
    io_mode = 3;
    assert(db->lookup(db, &address, &country, &error) == KNURL_EOF);
    assert(country.code[0] == 0 && reads == before);
    io_mode = 0;
    db->close(db, NULL);
    assert(live == 0 && peak <= limit);
    measuring = 0;
    printf("heap_limit=%lu opened_heap=%lu grown_heap=%lu peak_heap=%lu "
           "open_reads=%lu retained_after_close=%lu\n",
           (unsigned long)limit, (unsigned long)opened, (unsigned long)grown,
           (unsigned long)saved_peak, (unsigned long)open_reads,
           (unsigned long)live);
    return 0;
}
#endif
