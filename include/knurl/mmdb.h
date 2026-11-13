#ifndef KNURL_MMDB_H
#define KNURL_MMDB_H
#include <knurl/common.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Owned immutable country lookup provider. Close after all readers finish. */
typedef struct knurl_mmdb knurl_mmdb;
/** Network-order address: size is exactly 4 or 16; unused bytes are ignored. */
typedef struct knurl_mmdb_address {
    unsigned char bytes[16];
    unsigned int size;
} knurl_mmdb_address;
/** Copied uppercase two-letter country code followed by NUL. */
typedef struct knurl_mmdb_country {
    char code[3];
} knurl_mmdb_country;
/** Synchronous lookup; EOF means absent record/country, never a database error.
 * No DNS or whole-database decoding on this path. */
typedef knurl_status (*knurl_mmdb_lookup_fn)(const knurl_mmdb_address *,
                                             knurl_mmdb_country *, void *,
                                             knurl_error *);
/** Copied provider callbacks. Retain userdata until close; successful
 * construction arranges one close notification. Failure takes nothing. */
typedef struct knurl_mmdb_config {
    knurl_mmdb_lookup_fn lookup;
    /** Optional retirement callback, invoked once after readers finish. */
    void (*close)(void *);
    /** Context borrowed by lookup until its close notification. */
    void *userdata;
} knurl_mmdb_config;
/** Zero callbacks and context. */
KNURL_API void knurl_mmdb_config_init(knurl_mmdb_config *);
/** Create a provider. lookup is required; close is optional. */
KNURL_API knurl_status knurl_mmdb_new(const knurl_mmdb_config *, knurl_mmdb **,
                                      knurl_error *);
/** Parse only numeric IPv4/IPv6, without ports, scopes, CIDRs or DNS. Clears
 * destination on failure. IPv4-mapped IPv6 normalizes to IPv4 at lookup. */
KNURL_API knurl_status knurl_mmdb_address_parse(const char *,
                                                knurl_mmdb_address *,
                                                knurl_error *);
/** Copy country; EOF/errors clear output. Concurrent calls require a
 * thread-safe provider callback (the built-in file reader qualifies). */
KNURL_API knurl_status knurl_mmdb_lookup(knurl_mmdb *,
                                         const knurl_mmdb_address *,
                                         knurl_mmdb_country *, knurl_error *);
/** Release the file/cache and provider context. NULL is accepted. */
KNURL_API knurl_status knurl_mmdb_close(knurl_mmdb *, knurl_error *);
/** Default reader-owned requested heap limit, including cache bookkeeping. */
#define KNURL_MMDB_HEAP_DEFAULT 524288u
/** Minimum supported requested heap limit in bytes. */
#define KNURL_MMDB_HEAP_MIN 8192u
/** File-reader settings; zero values select defaults. */
typedef struct knurl_mmdb_file_config {
    /** Total requested reader heap bytes. Zero selects 512 KiB; minimum 8 KiB.
     * Includes handle/context/cache metadata, excludes allocator overhead and
     * kernel file cache. File blocks and prefix results allocate lazily and
     * evict within this shared bound. */
    size_t max_heap_bytes;
} knurl_mmdb_file_config;
/** Initialize zero/default file-reader settings. */
KNURL_API void knurl_mmdb_file_config_init(knurl_mmdb_file_config *);
/** Open an immutable regular file with bounded pread and lazy LRU caches for
 * file blocks and resolved country/unknown network prefixes.
 * NULL config selects defaults. Uses no mmap or eager whole-file staging.
 * Cache misses may block. Concurrent lookups serialize on this handle; close
 * only after readers finish. Does not require server initialization. */
KNURL_API knurl_status knurl_mmdb_open(const char *,
                                       const knurl_mmdb_file_config *,
                                       knurl_mmdb **, knurl_error *);
/** Library-owned receiver. Pass this handle as first argument. Do not copy,
 * initialize or modify the shell or impl. */
struct knurl_mmdb {
    knurl_status (*lookup)(knurl_mmdb *, const knurl_mmdb_address *,
                           knurl_mmdb_country *, knurl_error *);
    knurl_status (*close)(knurl_mmdb *, knurl_error *);
    struct knurl_mmdb_impl *impl;
};
#ifdef __cplusplus
}
#endif
#endif
