#include "storage_internal.h"
#include "asset_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct knurl_storage_provider {
    knurl_storage_provider_config config;
    size_t references, resources;
    knurl_loop *loop;
    unsigned long identity;
    long process;
    pthread_t thread;
    int closed;
};
struct knurl_storage_resource {
    knurl_storage_provider *provider;
    knurl_storage_resource_config config;
    knurl_storage_operation *owner, *pending;
    size_t references;
    int closed, busy;
};
struct knurl_storage_operation {
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
static void provider_ref(knurl_storage_provider *p) { ++p->references; }
static void provider_unref(void *data)
{
    knurl_storage_provider *p = data;
    if (--p->references == 0) {
        if (p->config.close != NULL)
            p->config.close(p->config.userdata);
        free((void *)p->config.commit_description);
        free(p);
    }
}
static knurl_status provider_owner(knurl_storage_provider *p,
                                   knurl_error *error)
{
    if (p == NULL)
        return fail(error, KNURL_INVALID, "missing storage provider");
    if (p->loop != NULL && (p->process != (long)getpid() ||
                            !pthread_equal(p->thread, pthread_self())))
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
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage provider");
    p->config = *config;
    p->config.commit_description = knurl_copy_text(config->commit_description);
    if (config->commit_description != NULL &&
        p->config.commit_description == NULL) {
        free(p);
        return fail(error, KNURL_NOMEM, "copying commit description");
    }
    p->references = 1;
    *out = p;
    knurl_error_clear(error);
    return KNURL_OK;
}
unsigned int
knurl_storage_provider_capabilities(const knurl_storage_provider *p)
{
    return p != NULL ? p->config.capabilities : 0;
}
const char *
knurl_storage_provider_commit_description(const knurl_storage_provider *p)
{
    return p != NULL ? p->config.commit_description : NULL;
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
    if (p->closed)
        return fail(error, KNURL_STATE, "storage provider is closed");
    p->closed = 1;
    provider_unref(p);
    knurl_error_clear(error);
    return KNURL_OK;
}
static void resource_ref(knurl_storage_resource *r)
{
    if (r != NULL)
        ++r->references;
}
static void resource_unref(knurl_storage_resource *r)
{
    knurl_storage_provider *p;
    if (r == NULL || --r->references != 0)
        return;
    p = r->provider;
    r->busy = 1;
    if (r->config.close != NULL)
        r->config.close(r->config.userdata);
    --p->resources;
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
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->retiring || op->acknowledged)
        return fail(error, KNURL_STATE, "storage operation already completed");
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage resource");
    r->config = *config;
    r->provider = op->provider;
    r->references = 1;
    ++r->provider->resources;
    provider_ref(r->provider);
    *out = r;
    knurl_error_clear(error);
    return KNURL_OK;
}
void *knurl_storage_resource_userdata(const knurl_storage_resource *r)
{
    return r != NULL ? r->config.userdata : NULL;
}
knurl_storage_resource_kind
knurl_storage_resource_type(const knurl_storage_resource *r)
{
    return r != NULL ? r->config.kind : KNURL_STORAGE_LIST;
}
knurl_status knurl_storage_resource_close(knurl_storage_resource *r,
                                          knurl_error *error)
{
    knurl_status status;
    if (r == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = provider_owner(r->provider, error);
    if (status != KNURL_OK)
        return status;
    if (r->busy || r->closed || r->owner != NULL)
        return fail(error, KNURL_STATE,
                    "storage resource is closed, busy or owned by a result");
    r->closed = 1;
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
    unsigned int caps = p->config.capabilities;
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
        if (a->resource == NULL || a->resource->provider != p ||
            a->resource->closed || a->resource->owner != NULL ||
            a->resource->pending != NULL ||
            a->resource->config.kind != (a->action == KNURL_STORAGE_LIST_NEXT
                                             ? KNURL_STORAGE_LIST
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
    if (op->result.asset != NULL) {
        knurl_asset_unclaim(op->result.asset, op);
        knurl_asset_close(op->result.asset, NULL);
    }
    if (op->result.resource != NULL) {
        op->result.resource->owner = NULL;
        knurl_storage_resource_close(op->result.resource, NULL);
    }
    op->result.asset = NULL;
    op->result.resource = NULL;
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
    knurl_storage_action action = op->arguments.action;
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
        (r->resource->provider != op->provider || r->resource->closed ||
         r->resource->owner != NULL ||
         r->resource->config.kind != (action == KNURL_STORAGE_LIST_OPEN
                                          ? KNURL_STORAGE_LIST
                                          : KNURL_STORAGE_WRITE)))
        return fail(error, KNURL_INVALID,
                    "result resource does not match provider/action");
    if (r->asset != NULL &&
        knurl_asset_stat(r->asset)->kind != KNURL_ASSET_FILE)
        return fail(error, KNURL_INVALID, "OPEN_READ must return a file asset");
    if (r->accepted != 0 &&
        (action != KNURL_STORAGE_PUT_WRITE || r->code != KNURL_STORAGE_OK ||
         r->accepted > op->arguments.bytes.length))
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
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->acknowledged || op->retiring || op->scope_busy)
        return fail(error, KNURL_STATE,
                    "storage operation already acknowledged");
    if (op->timer != NULL) {
        status = knurl_loop_monotonic(&acknowledged_at, error);
        if (status != KNURL_OK)
            return status;
        expired = acknowledged_at.high > op->timer->due.high ||
                  (acknowledged_at.high == op->timer->due.high &&
                   acknowledged_at.low >= op->timer->due.low);
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
                                   op->provider, error);
        if (status == KNURL_OK) {
            provider_ref(op->provider);
            copy.asset = result->asset;
        }
    }
    if (status != KNURL_OK) {
        result_metadata_clear(&copy);
        return status;
    }
    copy.resource = result->resource;
    if (copy.resource != NULL)
        copy.resource->owner = op;
    op->result = copy;
    op->acknowledged = 1;
    /* A queued timer is not proof its deadline has not elapsed. Completion
     * can arrive in the same native event batch before timer delivery. */
    if (expired) {
        if (op->cause == KNURL_OK)
            op->cause = KNURL_TIMEOUT;
        __atomic_store_n(&op->cancelled, 1, __ATOMIC_RELEASE);
    }
    if (op->arguments.resource != NULL && op->arguments.resource->pending == op)
        op->arguments.resource->pending = NULL;
    if (op->timer != NULL)
        knurl_timer_cancel(op->timer);
    (void)knurl_waker_signal(op->waker, NULL);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_loop *knurl_storage_operation_loop(const knurl_storage_operation *op)
{
    return op != NULL && knurl_loop_owner(op->loop, NULL) == KNURL_OK ? op->loop
                                                                      : NULL;
}
const knurl_storage_arguments *
knurl_storage_operation_arguments(const knurl_storage_operation *op)
{
    return op != NULL ? &op->arguments : NULL;
}
void *knurl_storage_operation_userdata(const knurl_storage_operation *op)
{
    return op != NULL ? op->userdata : NULL;
}
int knurl_storage_operation_cancelled(const knurl_storage_operation *op)
{
    return op == NULL || __atomic_load_n(&op->cancelled, __ATOMIC_ACQUIRE) != 0;
}
knurl_status knurl_storage_operation_set_userdata(knurl_storage_operation *op,
                                                  void *data,
                                                  knurl_error *error)
{
    knurl_status status;
    if (op == NULL)
        return fail(error, KNURL_INVALID, "missing storage operation");
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->retiring || op->acknowledged || op->userdata != NULL)
        return fail(error, KNURL_STATE,
                    "operation context is already set or retiring");
    op->userdata = data;
    knurl_error_clear(error);
    return KNURL_OK;
}
int knurl_storage_operation_has_scope_check(const knurl_storage_operation *op)
{
    return op != NULL && op->config.check_scope != NULL;
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
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (op->acknowledged || op->retiring || op->scope_busy)
        return fail(
            error, KNURL_STATE,
            "scope check requires a pending operation outside another check");
    if (knurl_storage_operation_cancelled(op))
        return fail(error, KNURL_CANCELLED, "storage policy access revoked");
    if (op->config.check_scope == NULL) {
        *allowed = 1;
        knurl_error_clear(error);
        return KNURL_OK;
    }
    op->scope_busy = 1;
    status = op->config.check_scope(op, path, destination, allowed,
                                    op->config.userdata, error);
    op->scope_busy = 0;
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
    if (--op->references != 0)
        return;
    outcome.cause = op->cause;
    outcome.cancelled = knurl_storage_operation_cancelled(op);
    outcome.result = &op->result;
    result_clear(op);
    if (op->accepted && op->provider->config.retired != NULL)
        op->provider->config.retired(op, &outcome,
                                     op->provider->config.userdata);
    if (op->accepted && op->config.retired != NULL)
        op->config.retired(&outcome, op->config.userdata);
    result_metadata_clear(&op->result);
    free((void *)op->arguments.path);
    free((void *)op->arguments.destination);
    free((void *)op->arguments.if_match);
    free((void *)op->arguments.if_none_match);
    free((void *)op->arguments.bytes.data);
    if (op->arguments.resource != NULL && op->arguments.resource->pending == op)
        op->arguments.resource->pending = NULL;
    resource_unref(op->arguments.resource);
    provider_unref(op->provider);
    free(op);
}
static void unlink_operation(knurl_storage_operation *op)
{
    knurl_storage_operation **p = &op->loop->storage_operations;
    if (!op->linked)
        return;
    while (*p != NULL && *p != op)
        p = &(*p)->next;
    if (*p == op)
        *p = op->next;
    op->linked = 0;
    --op->loop->storage_count;
}
static void operation_ready(knurl_loop *loop, void *data)
{
    knurl_storage_operation *op = data;
    knurl_storage_outcome outcome;
    (void)loop;
    if (op->retiring)
        return;
    if (knurl_storage_operation_cancelled(op) && !op->detached &&
        !op->interrupted) {
        op->interrupted = 1;
        if (op->config.interrupted != NULL)
            op->config.interrupted(op, op->cause, op->config.userdata);
    }
    if (!op->acknowledged)
        return;
    op->delivered = 1;
    outcome.cause = op->cause;
    outcome.cancelled = knurl_storage_operation_cancelled(op);
    outcome.result = &op->result;
    if (!outcome.cancelled && op->config.completed != NULL)
        op->config.completed(op, &outcome, op->config.userdata);
    op->retiring = 1;
    unlink_operation(op);
    if (op->timer != NULL)
        knurl_timer_cancel(op->timer);
    knurl_waker_destroy(op->waker, NULL);
    operation_unref(op);
}
static void protocol_failure(knurl_storage_operation *op, knurl_status status,
                             const knurl_error *native)
{
    knurl_storage_result result;
    if (op->acknowledged)
        return;
    knurl_storage_result_init(&result);
    result.code =
        status == KNURL_NOMEM ? KNURL_STORAGE_NOMEM : KNURL_STORAGE_IO;
    if (native != NULL)
        result.error = *native;
    if (result.error.status != status)
        fail(&result.error, status, "storage provider callback failed");
    if (op->cause == KNURL_OK)
        op->cause = status;
    (void)knurl_storage_operation_complete(op, &result, NULL);
}
static knurl_status cancel_operation(knurl_storage_operation *op,
                                     knurl_status cause, int detach,
                                     knurl_error *error)
{
    knurl_status status;
    knurl_error native;
    if (op->delivered) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (detach)
        op->detached = 1;
    if (__atomic_exchange_n(&op->cancelled, 1, __ATOMIC_ACQ_REL)) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (op->cause == KNURL_OK)
        op->cause = cause;
    (void)knurl_waker_signal(op->waker, NULL);
    if (op->acknowledged) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    knurl_error_clear(&native);
    status = op->provider->config.cancel(
        op, &op->arguments, op->provider->config.userdata, &native);
    if (status == KNURL_OK && !op->acknowledged)
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
    op->timer = NULL;
    operation_unref(op);
}
static void deadline(knurl_timer *timer, void *data)
{
    knurl_storage_operation *op = data;
    (void)timer;
    if (!op->acknowledged)
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
    status = knurl_loop_owner(op->loop, error);
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
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!op->acknowledged || !op->delivered || op->retiring ||
        knurl_storage_operation_cancelled(op) || op->result.asset == NULL)
        return fail(error, KNURL_STATE,
                    "asset may be taken only during successful completion");
    *out = op->result.asset;
    op->result.asset = NULL;
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
    status = knurl_loop_owner(op->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!op->acknowledged || !op->delivered || op->retiring ||
        knurl_storage_operation_cancelled(op) || op->result.resource == NULL)
        return fail(error, KNURL_STATE,
                    "resource may be taken only during successful completion");
    *out = op->result.resource;
    op->result.resource = NULL;
    (*out)->owner = NULL;
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status copy_arguments(knurl_storage_operation *op,
                                   const knurl_storage_arguments *a,
                                   knurl_error *error)
{
    knurl_storage_arguments *copy = &op->arguments;
    *copy = *a;
    copy->path = knurl_copy_text(a->path);
    copy->destination = knurl_copy_text(a->destination);
    copy->if_match = knurl_copy_text(a->if_match);
    copy->if_none_match = knurl_copy_text(a->if_none_match);
    copy->bytes.data = NULL;
    resource_ref(copy->resource);
    if (copy->resource != NULL)
        copy->resource->pending = op;
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
    if (!loop->native_ready || !loop->http_worker)
        return fail(
            error, KNURL_STATE,
            "storage operations require an active post-fork HTTP worker loop");
    if (!loop->accepting || loop->executor_stopping || p->closed)
        return fail(error, KNURL_STATE, "storage intake is closing");
    if (p->loop != NULL && (p->loop != loop || p->identity != loop->identity ||
                            p->process != (long)getpid()))
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
        if (!(p->config.capabilities & KNURL_STORAGE_CAN_SCOPE_CHECK))
            return fail(error, KNURL_NOT_SUPPORTED,
                        "provider cannot atomically check mutation scope");
    }
    if (defaults.timeout_ms < 0)
        return fail(error, KNURL_INVALID,
                    "storage timeout must be nonnegative; use timeout_disabled "
                    "for absence");
    if (defaults.timeout_ms == 0)
        defaults.timeout_ms = 30000;
    op = calloc(1, sizeof(*op));
    if (op == NULL)
        return fail(error, KNURL_NOMEM, "allocating storage operation");
    op->references = 1;
    op->loop = loop;
    op->provider = p;
    provider_ref(p);
    op->config = defaults;
    status = copy_arguments(op, args, error);
    if (status != KNURL_OK) {
        operation_unref(op);
        return status;
    }
    status = knurl_waker_new(loop, operation_ready, operation_unref, op,
                             &op->waker, error);
    if (status != KNURL_OK) {
        operation_unref(op);
        return status;
    }
    ++op->references;
    if (!defaults.timeout_disabled) {
        status = knurl_loop_timer(loop, defaults.timeout_ms, 0, deadline,
                                  deadline_retired, op, &op->timer, error);
        if (status != KNURL_OK) {
            knurl_waker_destroy(op->waker, NULL);
            operation_unref(op);
            return status;
        }
        ++op->references;
    }
    if (p->loop == NULL) {
        p->loop = loop;
        p->identity = loop->identity;
        p->process = (long)getpid();
        p->thread = pthread_self();
    }
    op->accepted = 1;
    op->next = loop->storage_operations;
    loop->storage_operations = op;
    op->linked = 1;
    ++loop->storage_count;
    *out = op;
    op->invoke_busy = 1;
    knurl_error_clear(&native);
    status = p->config.invoke(op, &op->arguments, p->config.userdata, &native);
    op->invoke_busy = 0;
    if (status == KNURL_OK && !op->acknowledged) {
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
    for (op = loop->storage_operations; op != NULL; op = next) {
        next = op->next;
        (void)cancel_operation(op, KNURL_CANCELLED, 0, NULL);
    }
}
