#include "acl_internal.h"
#include "server_internal.h"
#include "request_internal.h"
#include "executor.h"
#include "loop_internal.h"
#include <stdlib.h>

knurl_status knurl_route_set_acl(knurl_route *route, knurl_acl *acl,
                                 knurl_error *error)
{
    knurl_status status;
    knurl_acl *old;
    if (route == NULL || route->impl->router->impl->frozen)
        return knurl_fail(error, route ? KNURL_STATE : KNURL_INVALID,
                          "route.set_acl", "inactive route required");
    if (acl != NULL && (status = knurl_acl_retain(acl, error)) != KNURL_OK)
        return status;
    old = route->impl->config.acl;
    route->impl->config.acl = acl;
    return knurl_acl_close(old, error);
}
knurl_status knurl_route_group_set_acl(knurl_route_group *group, knurl_acl *acl,
                                       knurl_error *error)
{
    knurl_status status;
    knurl_acl *old;
    if (group == NULL || group->impl->router->impl->frozen)
        return knurl_fail(error, group ? KNURL_STATE : KNURL_INVALID,
                          "group.set_acl", "inactive group required");
    if (acl != NULL && (status = knurl_acl_retain(acl, error)) != KNURL_OK)
        return status;
    old = group->impl->config.acl;
    group->impl->config.acl = acl;
    return knurl_acl_close(old, error);
}
knurl_status knurl_acl_route_check(knurl_request *request, knurl_error *error)
{
    knurl_route *route = request->impl->route;
    knurl_acl *acls[2];
    knurl_mmdb_address peer, effective;
    knurl_status status;
    size_t i;
    int allowed;
    acls[0] = route->impl->group ? route->impl->group->impl->config.acl : NULL;
    acls[1] = route->impl->config.acl;
    if (acls[0] == NULL && acls[1] == NULL)
        return KNURL_OK;
    status = knurl_mmdb_address_parse(request->impl->peer.address.data, &peer,
                                      error);
    if (status == KNURL_OK)
        status = knurl_mmdb_address_parse(request->impl->effective_address.data,
                                          &effective, error);
    if (status != KNURL_OK) {
        request->impl->rejection_status = 503;
        return status;
    }
    for (i = 0; i < 2; ++i) {
        if (acls[i] == NULL)
            continue;
        if (request->impl->forwarding_invalid &&
            !acls[i]->impl->config.use_peer) {
            request->impl->rejection_status = 403;
            return knurl_fail(error, KNURL_INVALID, "acl.check",
                              "unusable forwarded client identity");
        }
        status = knurl_acl_check(acls[i], &peer, &effective, &allowed, error);
        if (status != KNURL_OK) {
            request->impl->rejection_status = 503;
            return status;
        }
        if (!allowed) {
            request->impl->rejection_status = 403;
            return knurl_fail(error, KNURL_CANCELLED, "acl.check",
                              "client denied by route ACL");
        }
    }
    return KNURL_OK;
}
static knurl_acl *route_acl(knurl_route *route, size_t level)
{
    return level ? route->impl->config.acl
                 : (route->impl->group ? route->impl->group->impl->config.acl
                                       : NULL);
}
knurl_status knurl_acl_router_prepare(knurl_server *server, knurl_error *error)
{
    knurl_router *router = server->impl->router;
    knurl_acl *acl;
    knurl_status status;
    size_t i, level;
    for (i = 0; i < router->impl->count; ++i)
        for (level = 0; level < 2; ++level) {
            acl = route_acl(router->impl->routes[i], level);
            if (acl == NULL)
                continue;
            status = knurl_acl_refresh(acl, error);
            if (status != KNURL_OK && status != KNURL_AGAIN)
                return status;
            (void)pthread_mutex_lock(&acl->impl->mutex);
            status = acl->impl->failure.status;
            if (status != KNURL_OK && error != NULL)
                *error = acl->impl->failure;
            (void)pthread_mutex_unlock(&acl->impl->mutex);
            if (status != KNURL_OK)
                return status;
        }
    return KNURL_OK;
}
struct refresh_item {
    knurl_acl *acl;
    knurl_executor *executor;
    size_t references;
    int busy;
    knurl_u64 deadline;
};
struct refresh_cancel {
    struct refresh_item *item;
    knurl_job *job;
};
static int refresh_cancelled(void *userdata)
{
    struct refresh_cancel *cancel = userdata;
    knurl_u64 now;
    int order;
    if (knurl_job_cancelled(cancel->job) ||
        knurl_loop_monotonic(&now, NULL) != KNURL_OK)
        return 1;
    (void)knurl_u64_compare(now, cancel->item->deadline, &order, NULL);
    return order >= 0;
}
static void item_release(void *userdata)
{
    struct refresh_item *item = userdata;
    if (--item->references == 0) {
        (void)knurl_acl_close(item->acl, NULL);
        free(item);
    }
}
static knurl_status refresh_work(knurl_job *job, knurl_bytes args,
                                 void *userdata, knurl_error *error)
{
    struct refresh_item *item = userdata;
    knurl_status status;
    struct refresh_cancel cancel;
    (void)args;
    if (knurl_job_cancelled(job))
        return KNURL_CANCELLED;
    cancel.item = item;
    cancel.job = job;
    status =
        knurl_acl_refresh_when(item->acl, refresh_cancelled, &cancel, error);
    return status == KNURL_AGAIN ? KNURL_OK : status;
}
static void refresh_interrupted(knurl_job *job, knurl_status status,
                                void *userdata)
{
    struct refresh_item *item = userdata;
    (void)job;
    if (status != KNURL_CANCELLED)
        knurl_acl_unavailable(item->acl, status,
                              "background DNS refresh deadline exceeded");
}
static void refresh_retired(const knurl_job_outcome *outcome, void *userdata)
{
    struct refresh_item *item = userdata;
    (void)outcome;
    item->busy = 0;
    item_release(item);
}
static void refresh_tick(knurl_timer *timer, void *userdata)
{
    struct refresh_item *item = userdata;
    knurl_job_config config;
    knurl_job *job;
    knurl_status status;
    knurl_error error;
    knurl_u64 now;
    (void)timer;
    if (item->busy || !knurl_acl_due(item->acl))
        return;
    if (knurl_loop_monotonic(&now, &error) != KNURL_OK ||
        knurl_loop_deadline(now, item->acl->impl->config.dns_timeout_ms,
                            &item->deadline, &error) != KNURL_OK) {
        knurl_acl_unavailable(item->acl, error.status,
                              "cannot determine DNS refresh deadline");
        return;
    }
    knurl_job_config_init(&config);
    config.work = refresh_work;
    config.interrupted = refresh_interrupted;
    config.retired = refresh_retired;
    config.userdata = item;
    config.timeout_ms = item->acl->impl->config.dns_timeout_ms;
    status = knurl_executor_submit(item->executor, &config, &job, &error);
    if (status == KNURL_OK) {
        ++item->references;
        item->busy = 1;
    } else
        knurl_acl_unavailable(item->acl, status,
                              "background DNS refresh queue unavailable");
}
static int dynamic(knurl_acl *acl)
{
    size_t i;
    if (acl == NULL || acl->impl->config.dns_refresh_ms == 0)
        return 0;
    for (i = 0; i < acl->impl->entry_count; ++i)
        if (acl->impl->entries[i].hostname != NULL)
            return 1;
    return 0;
}
knurl_status knurl_acl_worker_start(knurl_server *server, knurl_error *error)
{
    knurl_router *router = server->impl->router;
    knurl_acl **seen, *acl;
    knurl_executor *executor = NULL;
    knurl_executor_config config;
    struct refresh_item *item;
    knurl_timer *timer;
    knurl_status status = KNURL_OK;
    size_t i, j, level, count = 0;
    long repeat;
    if (router->impl->count > (size_t)-1 / (2 * sizeof(*seen)))
        return knurl_fail(error, KNURL_OVERFLOW, "acl.start",
                          "ACL registry overflow");
    seen = NULL;
    for (i = 0; i < router->impl->count; ++i)
        for (level = 0; level < 2; ++level) {
            acl = route_acl(router->impl->routes[i], level);
            if (!dynamic(acl))
                continue;
            if (seen == NULL) {
                seen = malloc(router->impl->count * 2 * sizeof(*seen));
                if (seen == NULL) {
                    status = knurl_fail(error, KNURL_NOMEM, "acl.start",
                                        "cannot allocate refresh registry");
                    goto done;
                }
            }
            for (j = 0; j < count; ++j)
                if (seen[j] == acl)
                    break;
            if (j != count)
                continue;
            seen[count++] = acl;
            if (knurl_acl_due(acl))
                knurl_acl_unavailable(acl, KNURL_AGAIN,
                                      "DNS refresh due at worker startup");
            if (executor == NULL) {
                knurl_executor_config_init(&config);
                status = knurl_executor_new(server, &config, &executor, error);
                if (status != KNURL_OK)
                    goto done;
            }
            item = calloc(1, sizeof(*item));
            if (item == NULL) {
                status = knurl_fail(error, KNURL_NOMEM, "acl.start",
                                    "cannot allocate refresh timer state");
                goto done;
            }
            status = knurl_acl_retain(acl, error);
            if (status != KNURL_OK) {
                free(item);
                goto done;
            }
            item->acl = acl;
            item->executor = executor;
            item->references = 1;
            repeat = acl->impl->config.dns_refresh_ms;
            if (repeat > 1000)
                repeat = 1000;
            status = knurl_loop_timer(knurl_server_loop(server), 0, repeat,
                                      refresh_tick, item_release, item, &timer,
                                      error);
            if (status != KNURL_OK) {
                item_release(item);
                goto done;
            }
        }
done:
    free(seen);
    return status;
}
