#ifndef KNURL_ACL_H
#define KNURL_ACL_H
#include <knurl/mmdb.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Reusable owned ACL. Routes retain their own references. */
typedef struct knurl_acl knurl_acl;
typedef enum knurl_acl_policy {
    KNURL_ACL_ALLOW = 0,
    KNURL_ACL_DENY = 1
} knurl_acl_policy;
/** Entries within a set are alternatives. All strings are copied. */
typedef struct knurl_acl_rules {
    const char *const *addresses;
    size_t address_count;
    const char *const *countries;
    size_t country_count;
    int unknown;
} knurl_acl_rules;
/** Forward-only resolver, called outside request handling. Fill at most
 * capacity normalized numeric addresses; report overflow as LIMIT, never
 * truncate. Dynamic resolvers must be thread-safe and must not access Lua or
 * HTTP handles. Retain userdata through ACL retirement. */
typedef knurl_status (*knurl_acl_resolve_fn)(const char *hostname,
                                             knurl_mmdb_address *addresses,
                                             size_t capacity, size_t *count,
                                             void *userdata,
                                             knurl_error *error);
typedef struct knurl_acl_config {
    knurl_acl_policy policy;
    knurl_acl_rules allow, deny;
    /** False selects effective client, true selects actual connection peer. */
    int use_peer;
    /** Zero resolves once; positive milliseconds opt into debounced refresh. */
    long dns_refresh_ms;
    /** Zero selects 64 addresses per hostname; maximum 1024. */
    size_t dns_max_addresses;
    /** Zero selects a 10 second background deadline. */
    long dns_timeout_ms;
    knurl_acl_resolve_fn resolve;
    void *resolve_userdata;
    /** Copied path, opened at preparation. Mutually exclusive with provider. */
    const char *country_database;
    /** Borrowed provider. Keep alive until every ACL reference is released. */
    knurl_mmdb *country_provider;
    /** Owned MMDB reader limit; zero selects its 512 KiB default. */
    size_t max_heap_bytes;
} knurl_acl_config;
KNURL_API void knurl_acl_config_init(knurl_acl_config *);
/** Inert construction. No DNS, files, threads or server initialization. */
KNURL_API knurl_status knurl_acl_new(const knurl_acl_config *, knurl_acl **,
                                     knurl_error *);
KNURL_API knurl_status knurl_acl_retain(knurl_acl *, knurl_error *);
/** Synchronous preparation/refresh for standalone hosts, never a request API.
 * AGAIN means not due or another refresh is running. Failures close admission
 * until a successful refresh; startup-only successful ACLs never resolve again.
 */
KNURL_API knurl_status knurl_acl_refresh(knurl_acl *, knurl_error *);
/** Never resolves DNS. NULL effective uses peer. Clears allowed on errors.
 * Unprepared/unavailable ACLs fail closed; callers must honor returned status.
 * Concurrent check/refresh is safe. Fork and final close require quiescent
 * users. */
KNURL_API knurl_status knurl_acl_check(knurl_acl *,
                                       const knurl_mmdb_address *peer,
                                       const knurl_mmdb_address *effective,
                                       int *allowed, knurl_error *);
/** Release one owned reference; NULL is accepted. */
KNURL_API knurl_status knurl_acl_close(knurl_acl *, knurl_error *);
struct knurl_acl {
    knurl_status (*retain)(knurl_acl *, knurl_error *);
    knurl_status (*refresh)(knurl_acl *, knurl_error *);
    knurl_status (*check)(knurl_acl *, const knurl_mmdb_address *,
                          const knurl_mmdb_address *, int *, knurl_error *);
    knurl_status (*close)(knurl_acl *, knurl_error *);
    struct knurl_acl_impl *impl;
};
#ifdef __cplusplus
}
#endif
#endif
