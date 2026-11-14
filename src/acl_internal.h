#ifndef KNURL_ACL_INTERNAL_H
#define KNURL_ACL_INTERNAL_H
#include "internal.h"
#include "ip.h"
#include <knurl/acl.h>
#include <pthread.h>
struct knurl_acl_entry {
    struct knurl_ip_network network;
    char *hostname;
    int deny;
};
struct knurl_acl_resolved {
    knurl_mmdb_address address;
    int deny;
};
struct knurl_acl_impl {
    knurl_acl receiver;
    pthread_mutex_t mutex, refresh_mutex;
    size_t references;
    knurl_acl_config config;
    struct knurl_acl_entry *entries;
    size_t entry_count;
    struct knurl_acl_resolved *resolved;
    size_t resolved_count;
    char *countries[2];
    knurl_mmdb *database;
    int own_database, prepared, attempted;
    knurl_u64 due;
    knurl_error failure;
};
knurl_status knurl_acl_resolve_system(const char *, knurl_mmdb_address *,
                                      size_t, size_t *, void *, knurl_error *);
knurl_status knurl_acl_resolve_all(knurl_acl *, struct knurl_acl_resolved **,
                                   size_t *, int (*)(void *), void *,
                                   knurl_error *);
int knurl_acl_due(knurl_acl *);
knurl_status knurl_acl_refresh_when(knurl_acl *, int (*)(void *), void *,
                                    knurl_error *);
void knurl_acl_unavailable(knurl_acl *, knurl_status, const char *);
struct knurl_server;
knurl_status knurl_acl_router_prepare(struct knurl_server *, knurl_error *);
knurl_status knurl_acl_worker_start(struct knurl_server *, knurl_error *);
struct knurl_request;
knurl_status knurl_acl_route_check(struct knurl_request *, knurl_error *);
#endif
