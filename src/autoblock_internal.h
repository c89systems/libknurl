#ifndef ROTMOS_AUTOBLOCK_INTERNAL_H
#define ROTMOS_AUTOBLOCK_INTERNAL_H
#include "server_internal.h"
#include <pthread.h>
#include <stdint.h>
/* Fixed Vectis rule bounds; no per-address allocations in the HTTP loop. */
struct rotmos_autoblock_entry {
    int used;
    char ip[46];
    uint64_t window, until;
    unsigned int counters[34];
};
struct rotmos_autoblock_shared {
#if defined(__linux__)
    pthread_mutex_t mutex;
#endif
    struct rotmos_autoblock_entry entries[1];
};
struct rotmos_autoblock {
    struct rotmos_autoblock_shared *shared;
    size_t bytes, capacity;
#if !defined(__linux__)
    int lock_fd;
    pthread_mutex_t mutex;
#endif
};
rotmos_status rotmos_autoblock_open(rotmos_server *, rotmos_error *);
void rotmos_autoblock_close(rotmos_server *);
int rotmos_autoblock_lock(struct rotmos_autoblock *);
void rotmos_autoblock_unlock(struct rotmos_autoblock *);
int rotmos_autoblock_allowed(rotmos_server *, rotmos_bytes);
void rotmos_autoblock_note(rotmos_server *, rotmos_bytes,
                           rotmos_observation_kind, int, const char *);
#endif
