/* Vectis's shared fixed-window autoblock table and worker-death lock recovery.
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "autoblock_internal.h"
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

knurl_status knurl_autoblock_open(knurl_server *server, knurl_error *error)
{
    struct knurl_autoblock *table;
    const knurl_autoblock_config *config = &server->impl->config.autoblock;
    int code;
#if defined(__linux__)
    pthread_mutexattr_t attr;
#else
    char path[] = "/tmp/knurl-autoblock-XXXXXX";
#endif
    if (!config->enabled)
        return KNURL_OK;
    table = calloc(1, sizeof(*table));
    if (table == NULL)
        return knurl_fail(error, KNURL_NOMEM, "autoblock", "allocating table");
    table->capacity = config->max_entries;
    table->bytes = sizeof(*table->shared) +
                   (table->capacity - 1) * sizeof(table->shared->entries[0]);
    table->shared = mmap(NULL, table->bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (table->shared == MAP_FAILED) {
        free(table);
        return knurl_fail(error, KNURL_IO, "autoblock", "mapping shared table");
    }
#if defined(__linux__)
    code = pthread_mutexattr_init(&attr);
    if (code == 0) {
        code = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (code == 0)
            code = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (code == 0)
            code = pthread_mutex_init(&table->shared->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }
#else
    table->lock_fd = mkstemp(path);
    code = table->lock_fd < 0;
    if (!code) {
        code = unlink(path) != 0;
        if (!code)
            code = fcntl(table->lock_fd, F_SETFD, FD_CLOEXEC) != 0;
        if (!code)
            code = pthread_mutex_init(&table->mutex, NULL);
        if (code)
            close(table->lock_fd);
    }
#endif
    if (code != 0) {
        munmap(table->shared, table->bytes);
        free(table);
        return knurl_fail(error, KNURL_IO, "autoblock",
                          "initializing recoverable shared lock");
    }
    server->impl->autoblock = table;
    return KNURL_OK;
}
void knurl_autoblock_close(knurl_server *server)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    if (table == NULL)
        return;
#if defined(__linux__)
    pthread_mutex_destroy(&table->shared->mutex);
#else
    close(table->lock_fd);
    pthread_mutex_destroy(&table->mutex);
#endif
    munmap(table->shared, table->bytes);
    free(table);
    server->impl->autoblock = NULL;
}
#if !defined(__linux__)
static int file_lock(int fd, short type)
{
    struct flock lock;
    int code;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    do {
        code = fcntl(fd, F_SETLKW, &lock);
    } while (code != 0 && errno == EINTR);
    return code == 0;
}
#endif
int knurl_autoblock_lock(struct knurl_autoblock *table)
{
#if defined(__linux__)
    int code = pthread_mutex_lock(&table->shared->mutex);
    if (code == EOWNERDEAD) {
        memset(table->shared->entries, 0,
               table->capacity * sizeof(table->shared->entries[0]));
        if (pthread_mutex_consistent(&table->shared->mutex) == 0)
            return 1;
        pthread_mutex_unlock(&table->shared->mutex);
    }
    return code == 0;
#else
    if (pthread_mutex_lock(&table->mutex) != 0)
        return 0;
    if (file_lock(table->lock_fd, F_WRLCK))
        return 1;
    pthread_mutex_unlock(&table->mutex);
    return 0;
#endif
}
void knurl_autoblock_unlock(struct knurl_autoblock *table)
{
#if defined(__linux__)
    pthread_mutex_unlock(&table->shared->mutex);
#else
    file_lock(table->lock_fd, F_UNLCK);
    pthread_mutex_unlock(&table->mutex);
#endif
}
static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000UL + (uint64_t)now.tv_nsec / 1000000UL;
}
static void reset(struct knurl_autoblock_entry *entry, const char *ip,
                  uint64_t now)
{
    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    strcpy(entry->ip, ip);
    entry->window = now;
}
static struct knurl_autoblock_entry *
find(struct knurl_autoblock *table, const char *ip, uint64_t now, int create)
{
    struct knurl_autoblock_entry *entry, *candidate = NULL;
    uint64_t oldest = (uint64_t)-1;
    size_t i;
    for (i = 0; i < table->capacity; ++i) {
        entry = &table->shared->entries[i];
        if (entry->used && !strcmp(entry->ip, ip))
            return entry;
        if (!entry->used) {
            candidate = entry;
            break;
        }
        if (entry->until <= now && entry->window < oldest) {
            candidate = entry;
            oldest = entry->window;
        }
    }
    if (!create || candidate == NULL)
        return NULL;
    reset(candidate, ip, now);
    return candidate;
}
static int ip_copy(knurl_bytes ip, char out[46])
{
    if (ip.data == NULL || ip.length == 0 || ip.length >= 46 ||
        memchr(ip.data, 0, ip.length) != NULL)
        return 0;
    memcpy(out, ip.data, ip.length);
    out[ip.length] = 0;
    return 1;
}
int knurl_autoblock_allowed(knurl_server *server, knurl_bytes ip)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    struct knurl_autoblock_entry *entry;
    char text[46];
    uint64_t now;
    int result;
    if (table == NULL || !ip_copy(ip, text) || (now = now_ms()) == 0 ||
        !knurl_autoblock_lock(table))
        return 1;
    entry = find(table, text, now, 0);
    result = entry == NULL || entry->until <= now;
    knurl_autoblock_unlock(table);
    return result;
}
static void note(struct knurl_autoblock *table,
                 const knurl_autoblock_config *config, const char *ip,
                 uint64_t now, size_t counter, unsigned int threshold)
{
    struct knurl_autoblock_entry *entry;
    uint64_t duration;
    if (threshold == 0 || (entry = find(table, ip, now, 1)) == NULL)
        return;
    if (now - entry->window > (uint64_t)config->window_seconds * 1000UL)
        reset(entry, ip, now);
    if (entry->counters[counter] < UINT_MAX)
        ++entry->counters[counter];
    duration = (uint64_t)config->block_seconds * 1000UL;
    if (entry->counters[counter] >= threshold)
        entry->until =
            duration > (uint64_t)-1 - now ? (uint64_t)-1 : now + duration;
}
void knurl_autoblock_note(knurl_server *server, knurl_bytes ip,
                          knurl_observation_kind kind, int status,
                          const char *name)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    const knurl_autoblock_config *config = &server->impl->config.autoblock;
    char text[46];
    size_t i;
    uint64_t now;
    if (table == NULL || !ip_copy(ip, text) || (now = now_ms()) == 0 ||
        !knurl_autoblock_lock(table))
        return;
    if (kind == KNURL_OBSERVE_TCP_STALL)
        note(table, config, text, now, 0, config->tcp_stall_threshold);
    else if (kind == KNURL_OBSERVE_TLS_FAILURE)
        note(table, config, text, now, 1, config->tls_failure_threshold);
    else if (kind == KNURL_OBSERVE_HTTP_STATUS) {
        for (i = 0; i < config->status_rule_count; ++i)
            if ((int)config->status_rules[i].status == status)
                note(table, config, text, now, 2 + i,
                     config->status_rules[i].threshold);
    } else if (kind == KNURL_OBSERVE_APPLICATION && name != NULL) {
        for (i = 0; i < config->event_rule_count; ++i)
            if (!strcmp(config->event_rules[i].name, name))
                note(table, config, text, now, 18 + i,
                     config->event_rules[i].threshold);
    }
    knurl_autoblock_unlock(table);
}
