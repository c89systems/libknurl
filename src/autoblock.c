/* Vectis's shared fixed-window autoblock table and worker-death lock recovery.
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "autoblock_internal.h"
#include "policy_internal.h"
#include "loop_internal.h"
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
    knurl_autoblock_history_clear(server);
    if (!config->enabled)
        return KNURL_OK;
    table = calloc(1, sizeof(*table));
    if (table == NULL)
        return knurl_fail(error, KNURL_NOMEM, "autoblock", "allocating table");
    table->capacity = config->max_entries;
    table->bytes = sizeof(*table->shared) +
                   (table->capacity - 1) * sizeof(table->shared->entries[0]);
    table->queue_capacity =
        server->impl->autoblock_notifications.queue_capacity;
    if (table->queue_capacity >
        ((size_t)-1 - table->bytes) / sizeof(knurl_autoblock_event)) {
        free(table);
        return knurl_fail(error, KNURL_OVERFLOW, "autoblock",
                          "event queue size overflow");
    }
    table->bytes += table->queue_capacity * sizeof(knurl_autoblock_event);
    table->shared = mmap(NULL, table->bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (table->shared == MAP_FAILED) {
        free(table);
        return knurl_fail(error, KNURL_IO, "autoblock", "mapping shared table");
    }
    table->events =
        (knurl_autoblock_event *)(table->shared->entries + table->capacity);
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
static void table_free(struct knurl_autoblock *table)
{
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
}
void knurl_autoblock_history_clear(knurl_server *server)
{
    table_free(server->impl->autoblock_history);
    server->impl->autoblock_history = NULL;
}
void knurl_autoblock_close(knurl_server *server)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    server->impl->autoblock = NULL;
    if (table != NULL && table->queue_capacity != 0) {
        knurl_autoblock_history_clear(server);
        server->impl->autoblock_history = table;
    } else
        table_free(table);
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
        if (table->shared->dropped > (uint64_t)-1 - table->shared->count)
            table->shared->dropped = (uint64_t)-1;
        else
            table->shared->dropped += table->shared->count;
        table->shared->head = table->shared->count = 0;
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
static knurl_u64 wide(uint64_t value)
{
    knurl_u64 result;
    result.high = (unsigned long)(value >> 32);
    result.low = (unsigned long)(value & 0xffffffffUL);
    return result;
}
static void text_copy(char out[46], knurl_bytes value)
{
    size_t size = value.length < 45 ? value.length : 45;
    if (value.data != NULL)
        memcpy(out, value.data, size);
    else
        size = 0;
    out[size] = 0;
}
static void context(knurl_server *server, knurl_autoblock_event *event,
                    knurl_request *request,
                    const knurl_connection_info *connection)
{
    if (request != NULL) {
        knurl_request_autoblock_context(request, event);
        return;
    }
    event->process = server->impl->process;
    event->listener_index = (size_t)-1;
    if (connection == NULL) {
        strcpy(event->peer_address, event->address);
        return;
    }
    event->process = connection->process;
    event->listener_index = connection->listener_index;
    event->tls = connection->tls;
    event->peer_port = connection->peer.port;
    event->local_port = connection->local.port;
    text_copy(event->peer_address, connection->peer.address);
    text_copy(event->local_address, connection->local.address);
}
static int note(knurl_server *server, const char *ip, uint64_t now,
                size_t counter, unsigned int threshold,
                knurl_autoblock_reason reason, int status, size_t rule,
                knurl_request *request, const knurl_connection_info *connection,
                knurl_autoblock_event *event)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    const knurl_autoblock_config *config = &server->impl->config.autoblock;
    struct knurl_autoblock_entry *entry;
    uint64_t duration, before;
    size_t length, slot;
    if (threshold == 0 || (entry = find(table, ip, now, 1)) == NULL)
        return 0;
    if (now - entry->window > (uint64_t)config->window_seconds * 1000UL)
        reset(entry, ip, now);
    if (entry->counters[counter] < UINT_MAX)
        ++entry->counters[counter];
    if (entry->counters[counter] < threshold)
        return 0;
    before = entry->until;
    duration = (uint64_t)config->block_seconds * 1000UL;
    entry->until =
        duration > (uint64_t)-1 - now ? (uint64_t)-1 : now + duration;
    if ((server->impl->autoblock_notifications.callback == NULL &&
         table->queue_capacity == 0) ||
        entry->until <= before)
        return 0;
    memset(event, 0, sizeof(*event));
    strcpy(event->address, ip);
    event->change =
        before <= now ? KNURL_AUTOBLOCK_BLOCKED : KNURL_AUTOBLOCK_EXTENDED;
    event->reason = reason;
    event->status = status;
    event->rule_index = rule;
    event->threshold = threshold;
    event->count = entry->counters[counter];
    event->window_seconds = config->window_seconds;
    event->occurred_at_ms = wide(now);
    event->expires_at_ms = wide(entry->until);
    event->duration_ms = wide(entry->until - now);
    if (reason == KNURL_AUTOBLOCK_APPLICATION) {
        length = strlen(config->event_rules[rule].name);
        event->name_truncated = length > KNURL_AUTOBLOCK_NAME_MAX;
        if (length > KNURL_AUTOBLOCK_NAME_MAX)
            length = KNURL_AUTOBLOCK_NAME_MAX;
        memcpy(event->name, config->event_rules[rule].name, length);
    }
    context(server, event, request, connection);
    if (table->queue_capacity != 0) {
        if (table->shared->count == table->queue_capacity) {
            table->shared->head =
                (table->shared->head + 1) % table->queue_capacity;
            --table->shared->count;
            if (table->shared->dropped != (uint64_t)-1)
                ++table->shared->dropped;
        }
        slot = (table->shared->head + table->shared->count) %
               table->queue_capacity;
        table->events[slot] = *event;
        ++table->shared->count;
    }
    return 1;
}
void knurl_autoblock_note(knurl_server *server, knurl_bytes ip,
                          knurl_observation_kind kind, int status,
                          const char *name, knurl_request *request,
                          const knurl_connection_info *connection)
{
    struct knurl_autoblock *table = server->impl->autoblock;
    const knurl_autoblock_config *config = &server->impl->config.autoblock;
    knurl_autoblock_event event;
    char text[46];
    size_t i;
    uint64_t now;
    int changed = 0;
    if (table == NULL || !ip_copy(ip, text) || (now = now_ms()) == 0 ||
        !knurl_autoblock_lock(table))
        return;
    if (kind == KNURL_OBSERVE_TCP_STALL)
        changed = note(server, text, now, 0, config->tcp_stall_threshold,
                       KNURL_AUTOBLOCK_TCP_STALL, 0, (size_t)-1, request,
                       connection, &event);
    else if (kind == KNURL_OBSERVE_TLS_FAILURE)
        changed = note(server, text, now, 1, config->tls_failure_threshold,
                       KNURL_AUTOBLOCK_TLS_FAILURE, 0, (size_t)-1, request,
                       connection, &event);
    else if (kind == KNURL_OBSERVE_HTTP_STATUS) {
        for (i = 0; i < config->status_rule_count; ++i)
            if ((int)config->status_rules[i].status == status) {
                changed |= note(server, text, now, 2 + i,
                                config->status_rules[i].threshold,
                                KNURL_AUTOBLOCK_HTTP_STATUS, status, i, request,
                                connection, &event);
            }
    } else if (kind == KNURL_OBSERVE_APPLICATION && name != NULL) {
        for (i = 0; i < config->event_rule_count; ++i)
            if (!strcmp(config->event_rules[i].name, name)) {
                changed |= note(server, text, now, 18 + i,
                                config->event_rules[i].threshold,
                                KNURL_AUTOBLOCK_APPLICATION, 0, i, request,
                                connection, &event);
            }
    }
    knurl_autoblock_unlock(table);
    if (changed && server->impl->autoblock_notifications.callback != NULL &&
        !server->impl->autoblock_notifying) {
        server->impl->autoblock_notifying = 1;
        server->impl->autoblock_notifications.callback(
            server, &event, server->impl->autoblock_notifications.userdata);
        server->impl->autoblock_notifying = 0;
    }
}
void knurl_autoblock_notifications_config_init(
    knurl_autoblock_notifications_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
knurl_status knurl_server_set_autoblock_notifications(
    knurl_server *server, const knurl_autoblock_notifications_config *config,
    knurl_error *error)
{
    if (server == NULL)
        return knurl_fail(error, KNURL_INVALID, "autoblock_notifications",
                          "server is required");
    if (knurl_server_running(server) || server->impl->autoblock != NULL ||
        server->impl->autoblock_notifying)
        return knurl_fail(error, KNURL_STATE, "autoblock_notifications",
                          "server must be inactive");
    if (config != NULL &&
        config->queue_capacity > (size_t)-1 / sizeof(knurl_autoblock_event))
        return knurl_fail(error, KNURL_OVERFLOW, "autoblock_notifications",
                          "queue capacity overflows size_t");
    knurl_autoblock_history_clear(server);
    knurl_autoblock_notifications_config_init(
        &server->impl->autoblock_notifications);
    if (config != NULL)
        server->impl->autoblock_notifications = *config;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_server_autoblock_next(knurl_server *server,
                                         knurl_autoblock_event *out,
                                         knurl_u64 *dropped, knurl_error *error)
{
    struct knurl_autoblock *table;
    knurl_status status = KNURL_EOF;
    if (server == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "autoblock_next",
                          "server and event output are required");
    if (knurl_server_running(server) && server->impl->loop != NULL) {
        status = knurl_loop_owner(server->impl->loop, error);
        if (status != KNURL_OK)
            return status;
        status = KNURL_EOF;
    }
    memset(out, 0, sizeof(*out));
    if (dropped != NULL)
        dropped->high = dropped->low = 0;
    table = server->impl->autoblock != NULL ? server->impl->autoblock
                                            : server->impl->autoblock_history;
    if (table == NULL)
        return knurl_fail(error, KNURL_EOF, "autoblock_next",
                          "notification queue is empty");
    if (!knurl_autoblock_lock(table))
        return knurl_fail(error, KNURL_STATE, "autoblock_next",
                          "cannot lock notification queue");
    if (dropped != NULL) {
        *dropped = wide(table->shared->dropped);
        table->shared->dropped = 0;
    }
    if (table->shared->count != 0) {
        *out = table->events[table->shared->head];
        table->shared->head = (table->shared->head + 1) % table->queue_capacity;
        --table->shared->count;
        status = KNURL_OK;
    }
    knurl_autoblock_unlock(table);
    if (status == KNURL_EOF)
        return knurl_fail(error, status, "autoblock_next",
                          "notification queue is empty");
    knurl_error_clear(error);
    return KNURL_OK;
}
