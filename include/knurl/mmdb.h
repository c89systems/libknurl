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
/** Thread-safe synchronous lookup; EOF means absent record/country, never a
 * database error. No DNS or whole-database decoding on this path. */
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
 * thread-safe provider callback (both built-in readers qualify). */
KNURL_API knurl_status knurl_mmdb_lookup(knurl_mmdb *,
                                         const knurl_mmdb_address *,
                                         knurl_mmdb_country *, knurl_error *);
/** Release mapping and provider context. NULL is accepted. */
KNURL_API knurl_status knurl_mmdb_close(knurl_mmdb *, knurl_error *);
/** Open a read-only immutable MMDB using embedded libmaxminddb. File I/O and
 * lazy mapping faults may block. Never modify a mapped file in place. */
KNURL_API knurl_status knurl_mmdb_maxmind_open(const char *, knurl_mmdb **,
                                               knurl_error *);
/** Open with Knurl's standalone C89 country reader. Same lifetime and lookup
 * contract as maxmind_open; neither constructor requires a webserver. */
KNURL_API knurl_status knurl_mmdb_native_open(const char *, knurl_mmdb **,
                                              knurl_error *);
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
