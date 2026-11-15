#include "storage_internal.h"
#include <knurl/storage.h>
#include "asset_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct knurl_storage_provider_impl {
    knurl_storage_provider receiver;
    knurl_storage_provider_config config;
    size_t references, resources;
    knurl_loop *loop;
    unsigned long identity;
    long process;
    pthread_t thread;
    int closed;
};
struct knurl_storage_resource_impl {
    knurl_storage_resource receiver;
    knurl_storage_provider *provider;
    knurl_storage_resource_config config;
    knurl_storage_operation *owner, *pending;
    size_t references;
    int closed, busy;
};
struct knurl_storage_operation_impl {
    knurl_storage_operation receiver;
    knurl_storage_provider *provider;
    knurl_loop *loop;
    knurl_storage_operation *next;
    knurl_storage_arguments arguments;
    knurl_storage_operation_config config;
    knurl_storage_result result;
    knurl_status cause;
    knurl_waker *waker;
    knurl_timer *timer;
    void *userdata;
    size_t references;
    unsigned int cancelled;
    int accepted, acknowledged, delivered, detached, interrupted, retiring,
        linked, invoke_busy, scope_busy;
};
static knurl_status fail(knurl_error *error, knurl_status status,
                         const char *message)
{
    return knurl_fail(error, status, "storage", message);
}
static int bounded(const char *s, size_t maximum)
{
    size_t n;
    if (s == NULL)
        return 1;
    for (n = 0; n <= maximum; ++n)
        if (s[n] == 0)
            return 1;
    return 0;
}
void knurl_storage_provider_config_init(knurl_storage_provider_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
void knurl_storage_operation_config_init(knurl_storage_operation_config *c)
{
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
        c->timeout_ms = 30000;
    }
}
void knurl_storage_arguments_init(knurl_storage_arguments *a)
{
    if (a != NULL)
        memset(a, 0, sizeof(*a));
}
void knurl_storage_result_init(knurl_storage_result *r)
{
    if (r != NULL)
        memset(r, 0, sizeof(*r));
}
void knurl_storage_resource_config_init(knurl_storage_resource_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
const char *knurl_storage_code_string(knurl_storage_code code)
{
    static const char *names[] = {
        "ok",         "not found",           "exists",        "invalid",
        "limit",      "I/O error",           "out of memory", "conflict",
        "tombstoned", "precondition failed", "not supported", "end",
        "denied"};
    return code >= KNURL_STORAGE_OK && code <= KNURL_STORAGE_DENIED
               ? names[code]
               : "unknown";
}
static void provider_ref(knurl_storage_provider *p) { ++p->impl->references; }
static void provider_unref(void *data)
{
    knurl_storage_provider *p = data;
    if (--p->impl->references == 0) {
        if (p->impl->config.close != NULL)
            p->impl->config.close(p->impl->config.userdata);
        free((void *)p->impl->config.commit_description);
        free(p);
    }
}
static knurl_status provider_owner(knurl_storage_provider *p,
                                   knurl_error *error)
{
    if (p == NULL)
        return fail(error, KNURL_INVALID, "missing storage provider");
    if (p->impl->loop != NULL &&
        (p->impl->process != (long)getpid() ||
         !pthread_equal(p->impl->thread, pthread_self())))
        return fail(error, KNURL_STATE,
                    "provider belongs to another process/thread");
    return KNURL_OK;
}
knurl_status
knurl_storage_provider_new(const knurl_storage_provider_config *config,
                           knurl_storage_provider **out, knurl_error *error)
{
    knurl_storage_provider *p;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->invoke == NULL ||
        config->cancel == NULL || (config->capabilities & ~127u) != 0 ||
        !bounded(config->commit_description, 256))
        return fail(error, KNURL_INVALID,
                    "provide invoke/cancel callbacks, valid capabilities and "
                    "bounded commit description");
    p = calloc(1, sizeof(struct knurl_storage_provider_impl));
    if (p == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage provider");
    knurl_storage_provider_receiver_init(p);
    p->impl->config = *config;
    p->impl->config.commit_description =
        knurl_copy_text(config->commit_description);
    if (config->commit_description != NULL &&
        p->impl->config.commit_description == NULL) {
        free(p);
        return fail(error, KNURL_NOMEM, "copying commit description");
    }
    p->impl->references = 1;
    *out = p;
    knurl_error_clear(error);
    return KNURL_OK;
}
unsigned int
knurl_storage_provider_capabilities(const knurl_storage_provider *p)
{
    return p != NULL ? p->impl->config.capabilities : 0;
}
const char *
knurl_storage_provider_commit_description(const knurl_storage_provider *p)
{
    return p != NULL ? p->impl->config.commit_description : NULL;
}
knurl_status knurl_storage_provider_close(knurl_storage_provider *p,
                                          knurl_error *error)
{
    knurl_status status;
    if (p == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = provider_owner(p, error);
    if (status != KNURL_OK)
        return status;
    if (p->impl->closed)
        return fail(error, KNURL_STATE, "storage provider is closed");
    p->impl->closed = 1;
    provider_unref(p);
    knurl_error_clear(error);
    return KNURL_OK;
}
static void resource_ref(knurl_storage_resource *r)
{
    if (r != NULL)
        ++r->impl->references;
}
static void resource_unref(knurl_storage_resource *r)
{
    knurl_storage_provider *p;
    if (r == NULL || --r->impl->references != 0)
        return;
    p = r->impl->provider;
    r->impl->busy = 1;
    if (r->impl->config.close != NULL)
        r->impl->config.close(r->impl->config.userdata);
    --p->impl->resources;
    free(r);
    provider_unref(p);
}
knurl_status
knurl_storage_resource_new(knurl_storage_operation *op,
                           const knurl_storage_resource_config *config,
                           knurl_storage_resource **out, knurl_error *error)
{
    knurl_storage_resource *r;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || config == NULL || out == NULL ||
        (config->kind != KNURL_STORAGE_LIST &&
         config->kind != KNURL_STORAGE_WRITE))
        return fail(error, KNURL_INVALID,
                    "provide operation, resource kind and output");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->impl->retiring || op->impl->acknowledged)
        return fail(error, KNURL_STATE, "storage operation already completed");
    r = calloc(1, sizeof(struct knurl_storage_resource_impl));
    if (r == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage resource");
    knurl_storage_resource_receiver_init(r);
    r->impl->config = *config;
    r->impl->provider = op->impl->provider;
    r->impl->references = 1;
    ++r->impl->provider->impl->resources;
    provider_ref(r->impl->provider);
    *out = r;
    knurl_error_clear(error);
    return KNURL_OK;
}
void *knurl_storage_resource_userdata(const knurl_storage_resource *r)
{
    return r != NULL ? r->impl->config.userdata : NULL;
}
knurl_storage_resource_kind
knurl_storage_resource_type(const knurl_storage_resource *r)
{
    return r != NULL ? r->impl->config.kind : KNURL_STORAGE_LIST;
}
knurl_status knurl_storage_resource_close(knurl_storage_resource *r,
                                          knurl_error *error)
{
    knurl_status status;
    if (r == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = provider_owner(r->impl->provider, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->busy || r->impl->closed || r->impl->owner != NULL)
        return fail(error, KNURL_STATE,
                    "storage resource is closed, busy or owned by a result");
    r->impl->closed = 1;
    resource_unref(r);
    knurl_error_clear(error);
    return KNURL_OK;
}
static int mutation(knurl_storage_action a)
{
    return a >= KNURL_STORAGE_PUT_BEGIN && a <= KNURL_STORAGE_MOVE;
}
static int commits(knurl_storage_action a)
{
    return a == KNURL_STORAGE_PUT_FINISH || a == KNURL_STORAGE_DELETE ||
           a == KNURL_STORAGE_MKCOL || a == KNURL_STORAGE_COPY ||
           a == KNURL_STORAGE_MOVE;
}
static knurl_status arguments_check(knurl_storage_provider *p,
                                    const knurl_storage_arguments *a,
                                    knurl_error *error)
{
    unsigned int caps = p->impl->config.capabilities;
    int resource_action;
    if (a == NULL || a->action < KNURL_STORAGE_STAT ||
        a->action > KNURL_STORAGE_MOVE ||
        a->bytes.length > KNURL_STORAGE_CHUNK_MAX ||
        (a->bytes.data == NULL && a->bytes.length != 0) ||
        !bounded(a->path, KNURL_ASSET_PATH_MAX) ||
        !bounded(a->destination, KNURL_ASSET_PATH_MAX) ||
        !bounded(a->if_match, KNURL_STORAGE_CONDITION_MAX) ||
        !bounded(a->if_none_match, KNURL_STORAGE_CONDITION_MAX))
        return fail(error, KNURL_INVALID,
                    "invalid storage action, condition or bounded chunk");
    resource_action = a->action == KNURL_STORAGE_LIST_NEXT ||
                      a->action == KNURL_STORAGE_PUT_WRITE ||
                      a->action == KNURL_STORAGE_PUT_FINISH ||
                      a->action == KNURL_STORAGE_PUT_ABORT;
    if (resource_action) {
        if (a->resource == NULL || a->resource->impl->provider != p ||
            a->resource->impl->closed || a->resource->impl->owner != NULL ||
            a->resource->impl->pending != NULL ||
            a->resource->impl->config.kind !=
                (a->action == KNURL_STORAGE_LIST_NEXT ? KNURL_STORAGE_LIST
                                                      : KNURL_STORAGE_WRITE))
            return fail(error, KNURL_INVALID,
                        "operation needs an open resource from this provider");
    } else if (a->resource != NULL || !knurl_asset_path_valid(a->path))
        return fail(error, KNURL_INVALID,
                    "operation requires a normalized confined path");
    if (a->action == KNURL_STORAGE_COPY || a->action == KNURL_STORAGE_MOVE) {
        if (!knurl_asset_path_valid(a->destination) ||
            (a->action == KNURL_STORAGE_COPY && a->depth != 0 &&
             a->depth != -1))
            return fail(
                error, KNURL_INVALID,
                "copy/move needs a confined destination and valid depth");
    }
    if ((a->action == KNURL_STORAGE_PUT_WRITE && a->bytes.length == 0) ||
        (a->action != KNURL_STORAGE_PUT_WRITE && a->bytes.length != 0))
        return fail(error, KNURL_INVALID,
                    "only PUT_WRITE accepts a nonempty bounded chunk");
    if (mutation(a->action) && !(caps & KNURL_STORAGE_CAN_WRITE))
        return fail(error, KNURL_NOT_SUPPORTED, "provider is read-only");
    if (a->action == KNURL_STORAGE_COPY && !(caps & KNURL_STORAGE_CAN_COPY))
        return fail(error, KNURL_NOT_SUPPORTED,
                    "provider does not support COPY");
    if (a->action == KNURL_STORAGE_MOVE && !(caps & KNURL_STORAGE_CAN_MOVE))
        return fail(error, KNURL_NOT_SUPPORTED,
                    "provider does not support MOVE");
    if (mutation(a->action) &&
        (a->if_match != NULL || a->if_none_match != NULL) &&
        !(caps & KNURL_STORAGE_CAN_CONDITIONAL))
        return fail(error, KNURL_NOT_SUPPORTED,
                    "provider cannot atomically enforce mutation conditions");
    if (a->atomic_cross_resource_required &&
        !(caps & KNURL_STORAGE_ATOMIC_CROSS_RESOURCE))
        return fail(error, KNURL_NOT_SUPPORTED,
                    "provider does not promise cross-resource atomicity");
    return KNURL_OK;
}
static void result_clear(knurl_storage_operation *op)
{
    if (op->impl->result.asset != NULL) {
        knurl_asset_unclaim(op->impl->result.asset, op);
        knurl_asset_close(op->impl->result.asset, NULL);
    }
    if (op->impl->result.resource != NULL) {
        op->impl->result.resource->impl->owner = NULL;
        knurl_storage_resource_close(op->impl->result.resource, NULL);
    }
    op->impl->result.asset = NULL;
    op->impl->result.resource = NULL;
}
static void result_metadata_clear(knurl_storage_result *r)
{
    knurl_asset_info_clear(&r->info);
    knurl_asset_info_clear(&r->entry.info);
    free((void *)r->entry.name);
    free((void *)r->recovery_path);
}
static knurl_status result_check(knurl_storage_operation *op,
                                 const knurl_storage_result *r, int expired,
                                 knurl_error *error)
{
    knurl_storage_action action = op->impl->arguments.action;
    if (r->code < KNURL_STORAGE_OK || r->code > KNURL_STORAGE_DENIED ||
        !bounded(r->recovery_path, KNURL_ASSET_PATH_MAX) ||
        (r->committed && !commits(action)) ||
        (r->partial && !mutation(action)) ||
        (r->has_entry && action != KNURL_STORAGE_LIST_NEXT) ||
        (r->asset != NULL && action != KNURL_STORAGE_OPEN_READ) ||
        (r->resource != NULL && action != KNURL_STORAGE_LIST_OPEN &&
         action != KNURL_STORAGE_PUT_BEGIN))
        return fail(error, KNURL_INVALID,
                    "result does not match storage action");
    if (r->resource != NULL &&
        (r->resource->impl->provider != op->impl->provider ||
         r->resource->impl->closed || r->resource->impl->owner != NULL ||
         r->resource->impl->config.kind != (action == KNURL_STORAGE_LIST_OPEN
                                                ? KNURL_STORAGE_LIST
                                                : KNURL_STORAGE_WRITE)))
        return fail(error, KNURL_INVALID,
                    "result resource does not match provider/action");
    if (r->asset != NULL &&
        knurl_asset_stat(r->asset)->kind != KNURL_ASSET_FILE)
        return fail(error, KNURL_INVALID, "OPEN_READ must return a file asset");
    if (r->accepted != 0 &&
        (action != KNURL_STORAGE_PUT_WRITE || r->code != KNURL_STORAGE_OK ||
         r->accepted > op->impl->arguments.bytes.length))
        return fail(error, KNURL_INVALID, "invalid accepted write prefix");
    if (!expired && !knurl_storage_operation_cancelled(op) &&
        r->code == KNURL_STORAGE_OK) {
        if ((action == KNURL_STORAGE_STAT && !r->has_info) ||
            (action == KNURL_STORAGE_OPEN_READ && r->asset == NULL) ||
            ((action == KNURL_STORAGE_LIST_OPEN ||
              action == KNURL_STORAGE_PUT_BEGIN) &&
             r->resource == NULL) ||
            (action == KNURL_STORAGE_LIST_NEXT && !r->has_entry) ||
            (action == KNURL_STORAGE_PUT_WRITE && r->accepted == 0) ||
            (commits(action) && !r->committed))
            return fail(error, KNURL_INVALID,
                        "successful result is missing metadata, resource, "
                        "accepted prefix or commit acknowledgement");
    }
    if (r->code == KNURL_STORAGE_END && action != KNURL_STORAGE_LIST_NEXT)
        return fail(error, KNURL_INVALID,
                    "END is only valid for listing iteration");
    return KNURL_OK;
}
knurl_status
knurl_storage_operation_complete(knurl_storage_operation *op,
                                 const knurl_storage_result *result,
                                 knurl_error *error)
{
    knurl_storage_result empty, copy;
    knurl_status status;
    knurl_u64 acknowledged_at;
    int expired = 0;
    if (op == NULL)
        return fail(error, KNURL_INVALID, "missing storage operation");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->impl->acknowledged || op->impl->retiring || op->impl->scope_busy)
        return fail(error, KNURL_STATE,
                    "storage operation already acknowledged");
    if (op->impl->timer != NULL) {
        status = knurl_loop_monotonic(&acknowledged_at, error);
        if (status != KNURL_OK)
            return status;
        expired = acknowledged_at.high > op->impl->timer->impl->due.high ||
                  (acknowledged_at.high == op->impl->timer->impl->due.high &&
                   acknowledged_at.low >= op->impl->timer->impl->due.low);
    }
    knurl_storage_result_init(&empty);
    if (result == NULL)
        result = &empty;
    status = result_check(op, result, expired, error);
    if (status != KNURL_OK)
        return status;
    copy = *result;
    memset(&copy.info, 0, sizeof(copy.info));
    memset(&copy.entry, 0, sizeof(copy.entry));
    copy.recovery_path = NULL;
    copy.asset = NULL;
    copy.resource = NULL;
    if (result->has_info)
        status = knurl_asset_info_copy(&result->info, &copy.info, error);
    if (status == KNURL_OK && result->has_entry) {
        if (!knurl_asset_child_valid(result->entry.name))
            status = fail(error, KNURL_INVALID, "invalid listing child name");
        else {
            status = knurl_asset_info_copy(&result->entry.info,
                                           &copy.entry.info, error);
            if (status == KNURL_OK) {
                copy.entry.name = knurl_copy_text(result->entry.name);
                if (copy.entry.name == NULL)
                    status = fail(error, KNURL_NOMEM, "copying listing child");
            }
        }
    }
    if (status == KNURL_OK && result->recovery_path != NULL) {
        copy.recovery_path = knurl_copy_text(result->recovery_path);
        if (copy.recovery_path == NULL)
            status = fail(error, KNURL_NOMEM, "copying recovery path");
    }
    if (status == KNURL_OK && result->asset != NULL) {
        status = knurl_asset_claim(result->asset, op, provider_unref,
                                   op->impl->provider, error);
        if (status == KNURL_OK) {
            provider_ref(op->impl->provider);
            copy.asset = result->asset;
        }
    }
    if (status != KNURL_OK) {
        result_metadata_clear(&copy);
        return status;
    }
    copy.resource = result->resource;
    if (copy.resource != NULL)
        copy.resource->impl->owner = op;
    op->impl->result = copy;
    op->impl->acknowledged = 1;
    /* A queued timer is not proof its deadline has not elapsed. Completion
     * can arrive in the same native event batch before timer delivery. */
    if (expired) {
        if (op->impl->cause == KNURL_OK)
            op->impl->cause = KNURL_TIMEOUT;
        __atomic_store_n(&op->impl->cancelled, 1, __ATOMIC_RELEASE);
    }
    if (op->impl->arguments.resource != NULL &&
        op->impl->arguments.resource->impl->pending == op)
        op->impl->arguments.resource->impl->pending = NULL;
    if (op->impl->timer != NULL)
        knurl_timer_cancel(op->impl->timer);
    (void)knurl_waker_signal(op->impl->waker, NULL);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_loop *knurl_storage_operation_loop(const knurl_storage_operation *op)
{
    return op != NULL && knurl_loop_owner(op->impl->loop, NULL) == KNURL_OK
               ? op->impl->loop
               : NULL;
}
const knurl_storage_arguments *
knurl_storage_operation_arguments(const knurl_storage_operation *op)
{
    return op != NULL ? &op->impl->arguments : NULL;
}
void *knurl_storage_operation_userdata(const knurl_storage_operation *op)
{
    return op != NULL ? op->impl->userdata : NULL;
}
int knurl_storage_operation_cancelled(const knurl_storage_operation *op)
{
    return op == NULL ||
           __atomic_load_n(&op->impl->cancelled, __ATOMIC_ACQUIRE) != 0;
}
knurl_status knurl_storage_operation_set_userdata(knurl_storage_operation *op,
                                                  void *data,
                                                  knurl_error *error)
{
    knurl_status status;
    if (op == NULL)
        return fail(error, KNURL_INVALID, "missing storage operation");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->impl->retiring || op->impl->acknowledged ||
        op->impl->userdata != NULL)
        return fail(error, KNURL_STATE,
                    "operation context is already set or retiring");
    op->impl->userdata = data;
    knurl_error_clear(error);
    return KNURL_OK;
}
int knurl_storage_operation_has_scope_check(const knurl_storage_operation *op)
{
    return op != NULL && op->impl->config.check_scope != NULL;
}
knurl_status knurl_storage_operation_check_scope(knurl_storage_operation *op,
                                                 const char *path,
                                                 const char *destination,
                                                 int *allowed,
                                                 knurl_error *error)
{
    knurl_status status;
    if (allowed != NULL)
        *allowed = 0;
    if (op == NULL || allowed == NULL || !knurl_asset_path_valid(path) ||
        (destination != NULL && !knurl_asset_path_valid(destination)))
        return fail(error, KNURL_INVALID,
                    "provide normalized scope paths and decision output");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->impl->acknowledged || op->impl->retiring || op->impl->scope_busy)
        return fail(
            error, KNURL_STATE,
            "scope check requires a pending operation outside another check");
    if (knurl_storage_operation_cancelled(op))
        return fail(error, KNURL_CANCELLED, "storage policy access revoked");
    if (op->impl->config.check_scope == NULL) {
        *allowed = 1;
        knurl_error_clear(error);
        return KNURL_OK;
    }
    op->impl->scope_busy = 1;
    status = op->impl->config.check_scope(op, path, destination, allowed,
                                          op->impl->config.userdata, error);
    op->impl->scope_busy = 0;
    if (status != KNURL_OK || knurl_storage_operation_cancelled(op))
        *allowed = 0;
    if (status == KNURL_OK && knurl_storage_operation_cancelled(op))
        return fail(error, KNURL_CANCELLED,
                    "storage policy access revoked during check");
    return status;
}
static void operation_unref(void *data)
{
    knurl_storage_operation *op = data;
    knurl_storage_outcome outcome;
    if (--op->impl->references != 0)
        return;
    outcome.cause = op->impl->cause;
    outcome.cancelled = knurl_storage_operation_cancelled(op);
    outcome.result = &op->impl->result;
    result_clear(op);
    if (op->impl->accepted && op->impl->provider->impl->config.retired != NULL)
        op->impl->provider->impl->config.retired(
            op, &outcome, op->impl->provider->impl->config.userdata);
    if (op->impl->accepted && op->impl->config.retired != NULL)
        op->impl->config.retired(&outcome, op->impl->config.userdata);
    result_metadata_clear(&op->impl->result);
    free((void *)op->impl->arguments.path);
    free((void *)op->impl->arguments.destination);
    free((void *)op->impl->arguments.if_match);
    free((void *)op->impl->arguments.if_none_match);
    free((void *)op->impl->arguments.bytes.data);
    if (op->impl->arguments.resource != NULL &&
        op->impl->arguments.resource->impl->pending == op)
        op->impl->arguments.resource->impl->pending = NULL;
    resource_unref(op->impl->arguments.resource);
    provider_unref(op->impl->provider);
    free(op);
}
static void unlink_operation(knurl_storage_operation *op)
{
    knurl_storage_operation **p = &op->impl->loop->impl->storage_operations;
    if (!op->impl->linked)
        return;
    while (*p != NULL && *p != op)
        p = &(*p)->impl->next;
    if (*p == op)
        *p = op->impl->next;
    op->impl->linked = 0;
    --op->impl->loop->impl->storage_count;
}
static void operation_ready(knurl_loop *loop, void *data)
{
    knurl_storage_operation *op = data;
    knurl_storage_outcome outcome;
    (void)loop;
    if (op->impl->retiring)
        return;
    if (knurl_storage_operation_cancelled(op) && !op->impl->detached &&
        !op->impl->interrupted) {
        op->impl->interrupted = 1;
        if (op->impl->config.interrupted != NULL)
            op->impl->config.interrupted(op, op->impl->cause,
                                         op->impl->config.userdata);
    }
    if (!op->impl->acknowledged)
        return;
    op->impl->delivered = 1;
    outcome.cause = op->impl->cause;
    outcome.cancelled = knurl_storage_operation_cancelled(op);
    outcome.result = &op->impl->result;
    if (!outcome.cancelled && op->impl->config.completed != NULL)
        op->impl->config.completed(op, &outcome, op->impl->config.userdata);
    op->impl->retiring = 1;
    unlink_operation(op);
    if (op->impl->timer != NULL)
        knurl_timer_cancel(op->impl->timer);
    knurl_waker_destroy(op->impl->waker, NULL);
    operation_unref(op);
}
static void protocol_failure(knurl_storage_operation *op, knurl_status status,
                             const knurl_error *native)
{
    knurl_storage_result result;
    if (op->impl->acknowledged)
        return;
    knurl_storage_result_init(&result);
    result.code =
        status == KNURL_NOMEM ? KNURL_STORAGE_NOMEM : KNURL_STORAGE_IO;
    if (native != NULL)
        result.error = *native;
    if (result.error.status != status)
        fail(&result.error, status, "storage provider callback failed");
    if (op->impl->cause == KNURL_OK)
        op->impl->cause = status;
    (void)knurl_storage_operation_complete(op, &result, NULL);
}
static knurl_status cancel_operation(knurl_storage_operation *op,
                                     knurl_status cause, int detach,
                                     knurl_error *error)
{
    knurl_status status;
    knurl_error native;
    if (op->impl->delivered) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (detach)
        op->impl->detached = 1;
    if (__atomic_exchange_n(&op->impl->cancelled, 1, __ATOMIC_ACQ_REL)) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (op->impl->cause == KNURL_OK)
        op->impl->cause = cause;
    (void)knurl_waker_signal(op->impl->waker, NULL);
    if (op->impl->acknowledged) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    knurl_error_clear(&native);
    status = op->impl->provider->impl->config.cancel(
        op, &op->impl->arguments, op->impl->provider->impl->config.userdata,
        &native);
    if (status == KNURL_OK && !op->impl->acknowledged)
        status = knurl_storage_operation_complete(op, NULL, &native);
    if (status == KNURL_OK || status == KNURL_AGAIN) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    /* A failed cancellation is not an acknowledgement. Keep the provider and
     * immutable arguments pinned until natural completion or process
     * escalation. */
    if (status < KNURL_OK || status > KNURL_BACKEND || status == KNURL_EOF)
        status = KNURL_INVALID;
    if (error != NULL)
        *error = native;
    return native.status == status
               ? status
               : fail(error, status,
                      "provider did not acknowledge cancellation");
}
static void deadline_retired(void *data)
{
    knurl_storage_operation *op = data;
    op->impl->timer = NULL;
    operation_unref(op);
}
static void deadline(knurl_timer *timer, void *data)
{
    knurl_storage_operation *op = data;
    (void)timer;
    if (!op->impl->acknowledged)
        (void)cancel_operation(op, KNURL_TIMEOUT, 0, NULL);
}
knurl_status knurl_storage_operation_cancel(knurl_storage_operation *op,
                                            knurl_error *error)
{
    knurl_status status;
    if (op == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    return cancel_operation(op, KNURL_CANCELLED, 1, error);
}
knurl_status knurl_storage_operation_take_asset(knurl_storage_operation *op,
                                                knurl_asset **out,
                                                knurl_error *error)
{
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || out == NULL)
        return fail(error, KNURL_INVALID, "provide operation and asset output");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!op->impl->acknowledged || !op->impl->delivered || op->impl->retiring ||
        knurl_storage_operation_cancelled(op) || op->impl->result.asset == NULL)
        return fail(error, KNURL_STATE,
                    "asset may be taken only during successful completion");
    *out = op->impl->result.asset;
    op->impl->result.asset = NULL;
    knurl_asset_unclaim(*out, op);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_storage_operation_take_resource(knurl_storage_operation *op,
                                                   knurl_storage_resource **out,
                                                   knurl_error *error)
{
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || out == NULL)
        return fail(error, KNURL_INVALID,
                    "provide operation and resource output");
    status = knurl_loop_owner(op->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!op->impl->acknowledged || !op->impl->delivered || op->impl->retiring ||
        knurl_storage_operation_cancelled(op) ||
        op->impl->result.resource == NULL)
        return fail(error, KNURL_STATE,
                    "resource may be taken only during successful completion");
    *out = op->impl->result.resource;
    op->impl->result.resource = NULL;
    (*out)->impl->owner = NULL;
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status copy_arguments(knurl_storage_operation *op,
                                   const knurl_storage_arguments *a,
                                   knurl_error *error)
{
    knurl_storage_arguments *copy = &op->impl->arguments;
    *copy = *a;
    copy->path = knurl_copy_text(a->path);
    copy->destination = knurl_copy_text(a->destination);
    copy->if_match = knurl_copy_text(a->if_match);
    copy->if_none_match = knurl_copy_text(a->if_none_match);
    copy->bytes.data = NULL;
    resource_ref(copy->resource);
    if (copy->resource != NULL)
        copy->resource->impl->pending = op;
    if (a->bytes.length != 0) {
        void *bytes = malloc(a->bytes.length);
        if (bytes != NULL)
            memcpy(bytes, a->bytes.data, a->bytes.length);
        copy->bytes.data = bytes;
    }
    if ((a->path != NULL && copy->path == NULL) ||
        (a->destination != NULL && copy->destination == NULL) ||
        (a->if_match != NULL && copy->if_match == NULL) ||
        (a->if_none_match != NULL && copy->if_none_match == NULL) ||
        (a->bytes.length != 0 && copy->bytes.data == NULL))
        return fail(error, KNURL_NOMEM, "copying pinned operation arguments");
    if (copy->action == KNURL_STORAGE_MOVE)
        copy->depth = -1;
    return KNURL_OK;
}
knurl_status
knurl_storage_operation_start(knurl_storage_provider *p, knurl_loop *loop,
                              const knurl_storage_arguments *args,
                              const knurl_storage_operation_config *config,
                              knurl_storage_operation **out, knurl_error *error)
{
    knurl_storage_operation *op;
    knurl_storage_operation_config defaults;
    knurl_status status;
    knurl_error native;
    if (out != NULL)
        *out = NULL;
    if (p == NULL || out == NULL)
        return fail(error, KNURL_INVALID,
                    "provide storage provider and operation output");
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (!loop->impl->native_ready || !loop->impl->http_worker)
        return fail(
            error, KNURL_STATE,
            "storage operations require an active post-fork HTTP worker loop");
    if (!loop->impl->accepting || loop->impl->executor_stopping ||
        p->impl->closed)
        return fail(error, KNURL_STATE, "storage intake is closing");
    if (p->impl->loop != NULL &&
        (p->impl->loop != loop || p->impl->identity != loop->impl->identity ||
         p->impl->process != (long)getpid()))
        return fail(error, KNURL_STATE,
                    "provider is bound to another loop/run");
    status = arguments_check(p, args, error);
    if (status != KNURL_OK)
        return status;
    knurl_storage_operation_config_init(&defaults);
    if (config != NULL)
        defaults = *config;
    if (defaults.check_scope != NULL) {
        if (args->action != KNURL_STORAGE_DELETE &&
            args->action != KNURL_STORAGE_COPY &&
            args->action != KNURL_STORAGE_MOVE)
            return fail(error, KNURL_INVALID,
                        "scope checks apply to DELETE/COPY/MOVE");
        if (!(p->impl->config.capabilities & KNURL_STORAGE_CAN_SCOPE_CHECK))
            return fail(error, KNURL_NOT_SUPPORTED,
                        "provider cannot atomically check mutation scope");
    }
    if (defaults.timeout_ms < 0)
        return fail(error, KNURL_INVALID,
                    "storage timeout must be nonnegative; use timeout_disabled "
                    "for absence");
    if (defaults.timeout_ms == 0)
        defaults.timeout_ms = 30000;
    op = calloc(1, sizeof(struct knurl_storage_operation_impl));
    if (op == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage operation");
    knurl_storage_operation_receiver_init(op);
    op->impl->references = 1;
    op->impl->loop = loop;
    op->impl->provider = p;
    provider_ref(p);
    op->impl->config = defaults;
    status = copy_arguments(op, args, error);
    if (status != KNURL_OK) {
        operation_unref(op);
        return status;
    }
    status = knurl_waker_new(loop, operation_ready, operation_unref, op,
                             &op->impl->waker, error);
    if (status != KNURL_OK) {
        operation_unref(op);
        return status;
    }
    ++op->impl->references;
    if (!defaults.timeout_disabled) {
        status =
            knurl_loop_timer(loop, defaults.timeout_ms, 0, deadline,
                             deadline_retired, op, &op->impl->timer, error);
        if (status != KNURL_OK) {
            knurl_waker_destroy(op->impl->waker, NULL);
            operation_unref(op);
            return status;
        }
        ++op->impl->references;
    }
    if (p->impl->loop == NULL) {
        p->impl->loop = loop;
        p->impl->identity = loop->impl->identity;
        p->impl->process = (long)getpid();
        p->impl->thread = pthread_self();
    }
    op->impl->accepted = 1;
    op->impl->next = loop->impl->storage_operations;
    loop->impl->storage_operations = op;
    op->impl->linked = 1;
    ++loop->impl->storage_count;
    *out = op;
    op->impl->invoke_busy = 1;
    knurl_error_clear(&native);
    status = p->impl->config.invoke(op, &op->impl->arguments,
                                    p->impl->config.userdata, &native);
    op->impl->invoke_busy = 0;
    if (status == KNURL_OK && !op->impl->acknowledged) {
        status = knurl_storage_operation_complete(op, NULL, &native);
        if (status != KNURL_OK)
            protocol_failure(op, status, &native);
    } else if (status != KNURL_OK && status != KNURL_AGAIN)
        protocol_failure(op, status, &native);
    knurl_error_clear(error);
    return KNURL_OK;
}
void knurl_storage_stopping(knurl_loop *loop)
{
    knurl_storage_operation *op, *next;
    if (loop == NULL)
        return;
    for (op = loop->impl->storage_operations; op != NULL; op = next) {
        next = op->impl->next;
        (void)cancel_operation(op, KNURL_CANCELLED, 0, NULL);
    }
}

void knurl_storage_provider_receiver_init(knurl_storage_provider *receiver)
{
    static const knurl_storage_provider methods = {
        knurl_storage_provider_capabilities,
        knurl_storage_provider_commit_description, knurl_storage_provider_close,
        knurl_storage_operation_start, NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_storage_provider_impl *)receiver;
}

void knurl_storage_operation_receiver_init(knurl_storage_operation *receiver)
{
    static const knurl_storage_operation methods = {
        knurl_storage_operation_complete,
        knurl_storage_operation_loop,
        knurl_storage_operation_arguments,
        knurl_storage_operation_userdata,
        knurl_storage_operation_set_userdata,
        knurl_storage_operation_has_scope_check,
        knurl_storage_operation_check_scope,
        knurl_storage_operation_cancelled,
        knurl_storage_operation_cancel,
        knurl_storage_operation_take_asset,
        knurl_storage_operation_take_resource,
        knurl_storage_resource_new,
        NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_storage_operation_impl *)receiver;
}

void knurl_storage_resource_receiver_init(knurl_storage_resource *receiver)
{
    static const knurl_storage_resource methods = {
        knurl_storage_resource_userdata, knurl_storage_resource_type,
        knurl_storage_resource_close, NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_storage_resource_impl *)receiver;
}
