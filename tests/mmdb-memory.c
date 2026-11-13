/* Linux linker wrappers measure requested live heap bytes of the reader,
 * including embedded libmaxminddb, without allocator/RSS noise. */
#include <knurl/mmdb.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
struct allocation {
    void *pointer;
    size_t size;
};
static struct allocation allocations[4096];
static size_t live, peak, calls;
static int measuring;
static void forget(void *pointer)
{
    size_t i;
    if (pointer == NULL)
        return;
    for (i = 0; i < 4096; ++i)
        if (allocations[i].pointer == pointer) {
            live -= allocations[i].size;
            allocations[i].pointer = NULL;
            return;
        }
}
static void *remember(void *pointer, size_t size)
{
    size_t i;
    if (!measuring || pointer == NULL)
        return pointer;
    for (i = 0; i < 4096; ++i)
        if (allocations[i].pointer == NULL) {
            allocations[i].pointer = pointer;
            allocations[i].size = size;
            live += size;
            if (live > peak)
                peak = live;
            ++calls;
            return pointer;
        }
    abort();
    return NULL;
}
void *__wrap_malloc(size_t size) { return remember(__real_malloc(size), size); }
void *__wrap_calloc(size_t count, size_t size)
{
    return remember(__real_calloc(count, size), count * size);
}
void *__wrap_realloc(void *pointer, size_t size)
{
    size_t index;
    void *result;
    index = 4096;
    if (pointer != NULL)
        for (index = 0; index < 4096; ++index)
            if (allocations[index].pointer == pointer)
                break;
    result = __real_realloc(pointer, size);
    if ((result != NULL || size == 0) && index < 4096) {
        live -= allocations[index].size;
        allocations[index].pointer = NULL;
    }
    return remember(result, size);
}
void __wrap_free(void *pointer)
{
    forget(pointer);
    __real_free(pointer);
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    size_t before, opened, i;
    assert(argc == 3);
    assert(knurl_mmdb_address_parse("1.2.3.4", &address, &error) == KNURL_OK);
    measuring = 1;
    status = strcmp(argv[1], "native") == 0
                 ? knurl_mmdb_native_open(argv[2], &db, &error)
                 : knurl_mmdb_maxmind_open(argv[2], &db, &error);
    assert(status == KNURL_OK);
    opened = live;
    before = calls;
    for (i = 0; i < 1000; ++i) {
        status = db->lookup(db, &address, &country, &error);
        assert(status == KNURL_OK || status == KNURL_EOF);
    }
    assert(calls == before && live == opened);
    db->close(db, NULL);
    assert(live == 0);
    measuring = 0;
    printf("%s reader_live_heap=%lu reader_peak_heap=%lu "
           "lookup_allocations=%lu retained_after_close=%lu\n",
           argv[1], (unsigned long)opened, (unsigned long)peak,
           (unsigned long)(calls - before), (unsigned long)live);
    return 0;
}
