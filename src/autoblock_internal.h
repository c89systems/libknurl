#ifndef KNURL_AUTOBLOCK_INTERNAL_H
#define KNURL_AUTOBLOCK_INTERNAL_H
#include "server_internal.h"
#include <pthread.h>
#include <stdint.h>
/* Fixed Vectis rule bounds; no per-address allocations in the HTTP loop. */
struct knurl_autoblock_entry {
    int used;
    char ip[46];
    uint64_t window, until;
    unsigned int counters[34];
};
struct knurl_autoblock_shared {
#if defined(__linux__)
    pthread_mutex_t mutex;
#endif
    size_t head, count;
    uint64_t dropped;
    struct knurl_autoblock_entry entries[1];
};
struct knurl_autoblock {
    struct knurl_autoblock_shared *shared;
    size_t bytes, capacity;
    size_t queue_capacity;
    knurl_autoblock_event *events;
#if !defined(__linux__)
    int lock_fd;
    pthread_mutex_t mutex;
#endif
};
knurl_status knurl_autoblock_open(knurl_server *, knurl_error *);
void knurl_autoblock_close(knurl_server *);
void knurl_autoblock_history_clear(knurl_server *);
int knurl_autoblock_lock(struct knurl_autoblock *);
void knurl_autoblock_unlock(struct knurl_autoblock *);
int knurl_autoblock_allowed(knurl_server *, knurl_bytes);
void knurl_autoblock_note(knurl_server *, knurl_bytes, knurl_observation_kind,
                          int, const char *, knurl_request *,
                          const knurl_connection_info *);
#endif
