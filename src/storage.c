#include "storage_internal.h"
#include "asset_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct rotmos_storage_provider {
    rotmos_storage_provider_config config;
    size_t references, resources;
    rotmos_loop *loop;
    unsigned long identity;
    long process;
    pthread_t thread;
    int closed;
};
struct rotmos_storage_resource {
    rotmos_storage_provider *provider;
    rotmos_storage_resource_config config;
    rotmos_storage_operation *owner, *pending;
    size_t references;
    int closed, busy;
};
struct rotmos_storage_operation {
    rotmos_storage_provider *provider;
    rotmos_loop *loop;
    rotmos_storage_operation *next;
    rotmos_storage_arguments arguments;
    rotmos_storage_operation_config config;
    rotmos_storage_result result;
    rotmos_status cause;
    rotmos_waker *waker;
    rotmos_timer *timer;
    void *userdata;
    size_t references;
    unsigned int cancelled;
    int accepted, acknowledged, delivered, detached, interrupted, retiring,
        linked, invoke_busy, scope_busy;
};
static rotmos_status fail(rotmos_error *error, rotmos_status status,
                          const char *message)
{
    return rotmos_fail(error, status, "storage", message);
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
void rotmos_storage_provider_config_init(rotmos_storage_provider_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
void rotmos_storage_operation_config_init(rotmos_storage_operation_config *c)
{
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
        c->timeout_ms = 30000;
    }
}
void rotmos_storage_arguments_init(rotmos_storage_arguments *a)
{
    if (a != NULL)
        memset(a, 0, sizeof(*a));
}
void rotmos_storage_result_init(rotmos_storage_result *r)
{
    if (r != NULL)
        memset(r, 0, sizeof(*r));
}
void rotmos_storage_resource_config_init(rotmos_storage_resource_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
const char *rotmos_storage_code_string(rotmos_storage_code code)
{
    static const char *names[] = {
        "ok",         "not found",           "exists",        "invalid",
        "limit",      "I/O error",           "out of memory", "conflict",
        "tombstoned", "precondition failed", "not supported", "end",
        "denied"};
    return code >= ROTMOS_STORAGE_OK && code <= ROTMOS_STORAGE_DENIED
               ? names[code]
               : "unknown";
}
static void provider_ref(rotmos_storage_provider *p) { ++p->references; }
static void provider_unref(void *data)
{
    rotmos_storage_provider *p = data;
    if (--p->references == 0) {
        if (p->config.close != NULL)
            p->config.close(p->config.userdata);
        free((void *)p->config.commit_description);
        free(p);
    }
}
static rotmos_status provider_owner(rotmos_storage_provider *p,
                                    rotmos_error *error)
{
    if (p == NULL)
        return fail(error, ROTMOS_INVALID, "missing storage provider");
    if (p->loop != NULL && (p->process != (long)getpid() ||
                            !pthread_equal(p->thread, pthread_self())))
        return fail(error, ROTMOS_STATE,
                    "provider belongs to another process/thread");
    return ROTMOS_OK;
}
rotmos_status
rotmos_storage_provider_new(const rotmos_storage_provider_config *config,
                            rotmos_storage_provider **out, rotmos_error *error)
{
    rotmos_storage_provider *p;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->invoke == NULL ||
        config->cancel == NULL || (config->capabilities & ~127u) != 0 ||
        !bounded(config->commit_description, 256))
        return fail(error, ROTMOS_INVALID,
                    "provide invoke/cancel callbacks, valid capabilities and "
                    "bounded commit description");
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return fail(error, ROTMOS_NOMEM, "allocating storage provider");
    p->config = *config;
    p->config.commit_description = rotmos_copy_text(config->commit_description);
    if (config->commit_description != NULL &&
        p->config.commit_description == NULL) {
        free(p);
        return fail(error, ROTMOS_NOMEM, "copying commit description");
    }
    p->references = 1;
    *out = p;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
unsigned int
rotmos_storage_provider_capabilities(const rotmos_storage_provider *p)
{
    return p != NULL ? p->config.capabilities : 0;
}
const char *
rotmos_storage_provider_commit_description(const rotmos_storage_provider *p)
{
    return p != NULL ? p->config.commit_description : NULL;
}
rotmos_status rotmos_storage_provider_close(rotmos_storage_provider *p,
                                            rotmos_error *error)
{
    rotmos_status status;
    if (p == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    status = provider_owner(p, error);
    if (status != ROTMOS_OK)
        return status;
    if (p->closed)
        return fail(error, ROTMOS_STATE, "storage provider is closed");
    p->closed = 1;
    provider_unref(p);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static void resource_ref(rotmos_storage_resource *r)
{
    if (r != NULL)
        ++r->references;
}
static void resource_unref(rotmos_storage_resource *r)
{
    rotmos_storage_provider *p;
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
rotmos_status
rotmos_storage_resource_new(rotmos_storage_operation *op,
                            const rotmos_storage_resource_config *config,
                            rotmos_storage_resource **out, rotmos_error *error)
{
    rotmos_storage_resource *r;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || config == NULL || out == NULL ||
        (config->kind != ROTMOS_STORAGE_LIST &&
         config->kind != ROTMOS_STORAGE_WRITE))
        return fail(error, ROTMOS_INVALID,
                    "provide operation, resource kind and output");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (op->retiring || op->acknowledged)
        return fail(error, ROTMOS_STATE, "storage operation already completed");
    if (op->provider->resources >= ROTMOS_STORAGE_MAX_RESOURCES)
        return fail(error, ROTMOS_LIMIT, "provider resource budget is full");
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return fail(error, ROTMOS_NOMEM, "allocating storage resource");
    r->config = *config;
    r->provider = op->provider;
    r->references = 1;
    ++r->provider->resources;
    provider_ref(r->provider);
    *out = r;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
void *rotmos_storage_resource_userdata(const rotmos_storage_resource *r)
{
    return r != NULL ? r->config.userdata : NULL;
}
rotmos_storage_resource_kind
rotmos_storage_resource_type(const rotmos_storage_resource *r)
{
    return r != NULL ? r->config.kind : ROTMOS_STORAGE_LIST;
}
rotmos_status rotmos_storage_resource_close(rotmos_storage_resource *r,
                                            rotmos_error *error)
{
    rotmos_status status;
    if (r == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    status = provider_owner(r->provider, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->busy || r->closed || r->owner != NULL)
        return fail(error, ROTMOS_STATE,
                    "storage resource is closed, busy or owned by a result");
    r->closed = 1;
    resource_unref(r);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static int mutation(rotmos_storage_action a)
{
    return a >= ROTMOS_STORAGE_PUT_BEGIN && a <= ROTMOS_STORAGE_MOVE;
}
static int commits(rotmos_storage_action a)
{
    return a == ROTMOS_STORAGE_PUT_FINISH || a == ROTMOS_STORAGE_DELETE ||
           a == ROTMOS_STORAGE_MKCOL || a == ROTMOS_STORAGE_COPY ||
           a == ROTMOS_STORAGE_MOVE;
}
static rotmos_status arguments_check(rotmos_storage_provider *p,
                                     const rotmos_storage_arguments *a,
                                     rotmos_error *error)
{
    unsigned int caps = p->config.capabilities;
    int resource_action;
    if (a == NULL || a->action < ROTMOS_STORAGE_STAT ||
        a->action > ROTMOS_STORAGE_MOVE ||
        a->bytes.length > ROTMOS_STORAGE_CHUNK_MAX ||
        (a->bytes.data == NULL && a->bytes.length != 0) ||
        !bounded(a->path, ROTMOS_ASSET_PATH_MAX) ||
        !bounded(a->destination, ROTMOS_ASSET_PATH_MAX) ||
        !bounded(a->if_match, ROTMOS_STORAGE_CONDITION_MAX) ||
        !bounded(a->if_none_match, ROTMOS_STORAGE_CONDITION_MAX))
        return fail(error, ROTMOS_INVALID,
                    "invalid storage action, condition or bounded chunk");
    resource_action = a->action == ROTMOS_STORAGE_LIST_NEXT ||
                      a->action == ROTMOS_STORAGE_PUT_WRITE ||
                      a->action == ROTMOS_STORAGE_PUT_FINISH ||
                      a->action == ROTMOS_STORAGE_PUT_ABORT;
    if (resource_action) {
        if (a->resource == NULL || a->resource->provider != p ||
            a->resource->closed || a->resource->owner != NULL ||
            a->resource->pending != NULL ||
            a->resource->config.kind != (a->action == ROTMOS_STORAGE_LIST_NEXT
                                             ? ROTMOS_STORAGE_LIST
                                             : ROTMOS_STORAGE_WRITE))
            return fail(error, ROTMOS_INVALID,
                        "operation needs an open resource from this provider");
    } else if (a->resource != NULL || !rotmos_asset_path_valid(a->path))
        return fail(error, ROTMOS_INVALID,
                    "operation requires a normalized confined path");
    if (a->action == ROTMOS_STORAGE_COPY || a->action == ROTMOS_STORAGE_MOVE) {
        if (!rotmos_asset_path_valid(a->destination) ||
            (a->action == ROTMOS_STORAGE_COPY && a->depth != 0 &&
             a->depth != -1))
            return fail(
                error, ROTMOS_INVALID,
                "copy/move needs a confined destination and valid depth");
    }
    if ((a->action == ROTMOS_STORAGE_PUT_WRITE && a->bytes.length == 0) ||
        (a->action != ROTMOS_STORAGE_PUT_WRITE && a->bytes.length != 0))
        return fail(error, ROTMOS_INVALID,
                    "only PUT_WRITE accepts a nonempty bounded chunk");
    if (mutation(a->action) && !(caps & ROTMOS_STORAGE_CAN_WRITE))
        return fail(error, ROTMOS_NOT_SUPPORTED, "provider is read-only");
    if (a->action == ROTMOS_STORAGE_COPY && !(caps & ROTMOS_STORAGE_CAN_COPY))
        return fail(error, ROTMOS_NOT_SUPPORTED,
                    "provider does not support COPY");
    if (a->action == ROTMOS_STORAGE_MOVE && !(caps & ROTMOS_STORAGE_CAN_MOVE))
        return fail(error, ROTMOS_NOT_SUPPORTED,
                    "provider does not support MOVE");
    if (mutation(a->action) &&
        (a->if_match != NULL || a->if_none_match != NULL) &&
        !(caps & ROTMOS_STORAGE_CAN_CONDITIONAL))
        return fail(error, ROTMOS_NOT_SUPPORTED,
                    "provider cannot atomically enforce mutation conditions");
    if (a->atomic_cross_resource_required &&
        !(caps & ROTMOS_STORAGE_ATOMIC_CROSS_RESOURCE))
        return fail(error, ROTMOS_NOT_SUPPORTED,
                    "provider does not promise cross-resource atomicity");
    return ROTMOS_OK;
}
static void result_clear(rotmos_storage_operation *op)
{
    if (op->result.asset != NULL) {
        rotmos_asset_unclaim(op->result.asset, op);
        rotmos_asset_close(op->result.asset, NULL);
    }
    if (op->result.resource != NULL) {
        op->result.resource->owner = NULL;
        rotmos_storage_resource_close(op->result.resource, NULL);
    }
    op->result.asset = NULL;
    op->result.resource = NULL;
}
static void result_metadata_clear(rotmos_storage_result *r)
{
    rotmos_asset_info_clear(&r->info);
    rotmos_asset_info_clear(&r->entry.info);
    free((void *)r->entry.name);
    free((void *)r->recovery_path);
}
static rotmos_status result_check(rotmos_storage_operation *op,
                                  const rotmos_storage_result *r,
                                  rotmos_error *error)
{
    rotmos_storage_action action = op->arguments.action;
    if (r->code < ROTMOS_STORAGE_OK || r->code > ROTMOS_STORAGE_DENIED ||
        !bounded(r->recovery_path, ROTMOS_ASSET_PATH_MAX) ||
        (r->committed && !commits(action)) ||
        (r->partial && !mutation(action)) ||
        (r->has_entry && action != ROTMOS_STORAGE_LIST_NEXT) ||
        (r->asset != NULL && action != ROTMOS_STORAGE_OPEN_READ) ||
        (r->resource != NULL && action != ROTMOS_STORAGE_LIST_OPEN &&
         action != ROTMOS_STORAGE_PUT_BEGIN))
        return fail(error, ROTMOS_INVALID,
                    "result does not match storage action");
    if (r->resource != NULL &&
        (r->resource->provider != op->provider || r->resource->closed ||
         r->resource->owner != NULL ||
         r->resource->config.kind != (action == ROTMOS_STORAGE_LIST_OPEN
                                          ? ROTMOS_STORAGE_LIST
                                          : ROTMOS_STORAGE_WRITE)))
        return fail(error, ROTMOS_INVALID,
                    "result resource does not match provider/action");
    if (r->asset != NULL &&
        rotmos_asset_stat(r->asset)->kind != ROTMOS_ASSET_FILE)
        return fail(error, ROTMOS_INVALID,
                    "OPEN_READ must return a file asset");
    if (r->accepted != 0 &&
        (action != ROTMOS_STORAGE_PUT_WRITE || r->code != ROTMOS_STORAGE_OK ||
         r->accepted > op->arguments.bytes.length))
        return fail(error, ROTMOS_INVALID, "invalid accepted write prefix");
    if (!rotmos_storage_operation_cancelled(op) &&
        r->code == ROTMOS_STORAGE_OK) {
        if ((action == ROTMOS_STORAGE_STAT && !r->has_info) ||
            (action == ROTMOS_STORAGE_OPEN_READ && r->asset == NULL) ||
            ((action == ROTMOS_STORAGE_LIST_OPEN ||
              action == ROTMOS_STORAGE_PUT_BEGIN) &&
             r->resource == NULL) ||
            (action == ROTMOS_STORAGE_LIST_NEXT && !r->has_entry) ||
            (action == ROTMOS_STORAGE_PUT_WRITE && r->accepted == 0) ||
            (commits(action) && !r->committed))
            return fail(error, ROTMOS_INVALID,
                        "successful result is missing metadata, resource, "
                        "accepted prefix or commit acknowledgement");
    }
    if (r->code == ROTMOS_STORAGE_END && action != ROTMOS_STORAGE_LIST_NEXT)
        return fail(error, ROTMOS_INVALID,
                    "END is only valid for listing iteration");
    return ROTMOS_OK;
}
rotmos_status
rotmos_storage_operation_complete(rotmos_storage_operation *op,
                                  const rotmos_storage_result *result,
                                  rotmos_error *error)
{
    rotmos_storage_result empty, copy;
    rotmos_status status;
    if (op == NULL)
        return fail(error, ROTMOS_INVALID, "missing storage operation");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (op->acknowledged || op->retiring || op->scope_busy)
        return fail(error, ROTMOS_STATE,
                    "storage operation already acknowledged");
    rotmos_storage_result_init(&empty);
    if (result == NULL)
        result = &empty;
    status = result_check(op, result, error);
    if (status != ROTMOS_OK)
        return status;
    copy = *result;
    memset(&copy.info, 0, sizeof(copy.info));
    memset(&copy.entry, 0, sizeof(copy.entry));
    copy.recovery_path = NULL;
    copy.asset = NULL;
    copy.resource = NULL;
    if (result->has_info)
        status = rotmos_asset_info_copy(&result->info, &copy.info, error);
    if (status == ROTMOS_OK && result->has_entry) {
        if (!rotmos_asset_child_valid(result->entry.name))
            status = fail(error, ROTMOS_INVALID, "invalid listing child name");
        else {
            status = rotmos_asset_info_copy(&result->entry.info,
                                            &copy.entry.info, error);
            if (status == ROTMOS_OK) {
                copy.entry.name = rotmos_copy_text(result->entry.name);
                if (copy.entry.name == NULL)
                    status = fail(error, ROTMOS_NOMEM, "copying listing child");
            }
        }
    }
    if (status == ROTMOS_OK && result->recovery_path != NULL) {
        copy.recovery_path = rotmos_copy_text(result->recovery_path);
        if (copy.recovery_path == NULL)
            status = fail(error, ROTMOS_NOMEM, "copying recovery path");
    }
    if (status == ROTMOS_OK && result->asset != NULL) {
        status = rotmos_asset_claim(result->asset, op, provider_unref,
                                    op->provider, error);
        if (status == ROTMOS_OK) {
            provider_ref(op->provider);
            copy.asset = result->asset;
        }
    }
    if (status != ROTMOS_OK) {
        result_metadata_clear(&copy);
        return status;
    }
    copy.resource = result->resource;
    if (copy.resource != NULL)
        copy.resource->owner = op;
    op->result = copy;
    op->acknowledged = 1;
    if (op->arguments.resource != NULL && op->arguments.resource->pending == op)
        op->arguments.resource->pending = NULL;
    if (op->timer != NULL)
        rotmos_timer_cancel(op->timer);
    (void)rotmos_waker_signal(op->waker, NULL);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_loop *rotmos_storage_operation_loop(const rotmos_storage_operation *op)
{
    return op != NULL && rotmos_loop_owner(op->loop, NULL) == ROTMOS_OK
               ? op->loop
               : NULL;
}
const rotmos_storage_arguments *
rotmos_storage_operation_arguments(const rotmos_storage_operation *op)
{
    return op != NULL ? &op->arguments : NULL;
}
void *rotmos_storage_operation_userdata(const rotmos_storage_operation *op)
{
    return op != NULL ? op->userdata : NULL;
}
int rotmos_storage_operation_cancelled(const rotmos_storage_operation *op)
{
    return op == NULL || __atomic_load_n(&op->cancelled, __ATOMIC_ACQUIRE) != 0;
}
rotmos_status
rotmos_storage_operation_set_userdata(rotmos_storage_operation *op, void *data,
                                      rotmos_error *error)
{
    rotmos_status status;
    if (op == NULL)
        return fail(error, ROTMOS_INVALID, "missing storage operation");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (op->retiring || op->acknowledged || op->userdata != NULL)
        return fail(error, ROTMOS_STATE,
                    "operation context is already set or retiring");
    op->userdata = data;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
int rotmos_storage_operation_has_scope_check(const rotmos_storage_operation *op)
{
    return op != NULL && op->config.check_scope != NULL;
}
rotmos_status rotmos_storage_operation_check_scope(rotmos_storage_operation *op,
                                                   const char *path,
                                                   const char *destination,
                                                   int *allowed,
                                                   rotmos_error *error)
{
    rotmos_status status;
    if (allowed != NULL)
        *allowed = 0;
    if (op == NULL || allowed == NULL || !rotmos_asset_path_valid(path) ||
        (destination != NULL && !rotmos_asset_path_valid(destination)))
        return fail(error, ROTMOS_INVALID,
                    "provide normalized scope paths and decision output");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (op->acknowledged || op->retiring || op->scope_busy)
        return fail(
            error, ROTMOS_STATE,
            "scope check requires a pending operation outside another check");
    if (rotmos_storage_operation_cancelled(op))
        return fail(error, ROTMOS_CANCELLED, "storage policy access revoked");
    if (op->config.check_scope == NULL) {
        *allowed = 1;
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    op->scope_busy = 1;
    status = op->config.check_scope(op, path, destination, allowed,
                                    op->config.userdata, error);
    op->scope_busy = 0;
    if (status != ROTMOS_OK || rotmos_storage_operation_cancelled(op))
        *allowed = 0;
    if (status == ROTMOS_OK && rotmos_storage_operation_cancelled(op))
        return fail(error, ROTMOS_CANCELLED,
                    "storage policy access revoked during check");
    return status;
}
static void operation_unref(void *data)
{
    rotmos_storage_operation *op = data;
    rotmos_storage_outcome outcome;
    if (--op->references != 0)
        return;
    outcome.cause = op->cause;
    outcome.cancelled = rotmos_storage_operation_cancelled(op);
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
static void unlink_operation(rotmos_storage_operation *op)
{
    rotmos_storage_operation **p = &op->loop->storage_operations;
    if (!op->linked)
        return;
    while (*p != NULL && *p != op)
        p = &(*p)->next;
    if (*p == op)
        *p = op->next;
    op->linked = 0;
    --op->loop->storage_count;
}
static void operation_ready(rotmos_loop *loop, void *data)
{
    rotmos_storage_operation *op = data;
    rotmos_storage_outcome outcome;
    (void)loop;
    if (op->retiring)
        return;
    if (rotmos_storage_operation_cancelled(op) && !op->detached &&
        !op->interrupted) {
        op->interrupted = 1;
        if (op->config.interrupted != NULL)
            op->config.interrupted(op, op->cause, op->config.userdata);
    }
    if (!op->acknowledged)
        return;
    op->delivered = 1;
    outcome.cause = op->cause;
    outcome.cancelled = rotmos_storage_operation_cancelled(op);
    outcome.result = &op->result;
    if (!outcome.cancelled && op->config.completed != NULL)
        op->config.completed(op, &outcome, op->config.userdata);
    op->retiring = 1;
    unlink_operation(op);
    if (op->timer != NULL)
        rotmos_timer_cancel(op->timer);
    rotmos_waker_destroy(op->waker, NULL);
    operation_unref(op);
}
static void protocol_failure(rotmos_storage_operation *op, rotmos_status status,
                             const rotmos_error *native)
{
    rotmos_storage_result result;
    if (op->acknowledged)
        return;
    rotmos_storage_result_init(&result);
    result.code =
        status == ROTMOS_NOMEM ? ROTMOS_STORAGE_NOMEM : ROTMOS_STORAGE_IO;
    if (native != NULL)
        result.error = *native;
    if (result.error.status != status)
        fail(&result.error, status, "storage provider callback failed");
    if (op->cause == ROTMOS_OK)
        op->cause = status;
    (void)rotmos_storage_operation_complete(op, &result, NULL);
}
static rotmos_status cancel_operation(rotmos_storage_operation *op,
                                      rotmos_status cause, int detach,
                                      rotmos_error *error)
{
    rotmos_status status;
    rotmos_error native;
    if (op->delivered) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (detach)
        op->detached = 1;
    if (__atomic_exchange_n(&op->cancelled, 1, __ATOMIC_ACQ_REL)) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (op->cause == ROTMOS_OK)
        op->cause = cause;
    (void)rotmos_waker_signal(op->waker, NULL);
    if (op->acknowledged) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    rotmos_error_clear(&native);
    status = op->provider->config.cancel(
        op, &op->arguments, op->provider->config.userdata, &native);
    if (status == ROTMOS_OK && !op->acknowledged)
        status = rotmos_storage_operation_complete(op, NULL, &native);
    if (status == ROTMOS_OK || status == ROTMOS_AGAIN) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    /* A failed cancellation is not an acknowledgement. Keep the provider and
     * immutable arguments pinned until natural completion or process
     * escalation. */
    if (status < ROTMOS_OK || status > ROTMOS_BACKEND || status == ROTMOS_EOF)
        status = ROTMOS_INVALID;
    if (error != NULL)
        *error = native;
    return native.status == status
               ? status
               : fail(error, status,
                      "provider did not acknowledge cancellation");
}
static void deadline_retired(void *data)
{
    rotmos_storage_operation *op = data;
    op->timer = NULL;
    operation_unref(op);
}
static void deadline(rotmos_timer *timer, void *data)
{
    rotmos_storage_operation *op = data;
    (void)timer;
    if (!op->acknowledged)
        (void)cancel_operation(op, ROTMOS_TIMEOUT, 0, NULL);
}
rotmos_status rotmos_storage_operation_cancel(rotmos_storage_operation *op,
                                              rotmos_error *error)
{
    rotmos_status status;
    if (op == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    return cancel_operation(op, ROTMOS_CANCELLED, 1, error);
}
rotmos_status rotmos_storage_operation_take_asset(rotmos_storage_operation *op,
                                                  rotmos_asset **out,
                                                  rotmos_error *error)
{
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || out == NULL)
        return fail(error, ROTMOS_INVALID,
                    "provide operation and asset output");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!op->acknowledged || !op->delivered || op->retiring ||
        rotmos_storage_operation_cancelled(op) || op->result.asset == NULL)
        return fail(error, ROTMOS_STATE,
                    "asset may be taken only during successful completion");
    *out = op->result.asset;
    op->result.asset = NULL;
    rotmos_asset_unclaim(*out, op);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status
rotmos_storage_operation_take_resource(rotmos_storage_operation *op,
                                       rotmos_storage_resource **out,
                                       rotmos_error *error)
{
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (op == NULL || out == NULL)
        return fail(error, ROTMOS_INVALID,
                    "provide operation and resource output");
    status = rotmos_loop_owner(op->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!op->acknowledged || !op->delivered || op->retiring ||
        rotmos_storage_operation_cancelled(op) || op->result.resource == NULL)
        return fail(error, ROTMOS_STATE,
                    "resource may be taken only during successful completion");
    *out = op->result.resource;
    op->result.resource = NULL;
    (*out)->owner = NULL;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static rotmos_status copy_arguments(rotmos_storage_operation *op,
                                    const rotmos_storage_arguments *a,
                                    rotmos_error *error)
{
    rotmos_storage_arguments *copy = &op->arguments;
    *copy = *a;
    copy->path = rotmos_copy_text(a->path);
    copy->destination = rotmos_copy_text(a->destination);
    copy->if_match = rotmos_copy_text(a->if_match);
    copy->if_none_match = rotmos_copy_text(a->if_none_match);
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
        return fail(error, ROTMOS_NOMEM, "copying pinned operation arguments");
    if (copy->action == ROTMOS_STORAGE_MOVE)
        copy->depth = -1;
    return ROTMOS_OK;
}
rotmos_status
rotmos_storage_operation_start(rotmos_storage_provider *p, rotmos_loop *loop,
                               const rotmos_storage_arguments *args,
                               const rotmos_storage_operation_config *config,
                               rotmos_storage_operation **out,
                               rotmos_error *error)
{
    rotmos_storage_operation *op;
    rotmos_storage_operation_config defaults;
    rotmos_status status;
    rotmos_error native;
    if (out != NULL)
        *out = NULL;
    if (p == NULL || out == NULL)
        return fail(error, ROTMOS_INVALID,
                    "provide storage provider and operation output");
    status = rotmos_loop_owner(loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!loop->native_ready || !loop->http_worker)
        return fail(
            error, ROTMOS_STATE,
            "storage operations require an active post-fork HTTP worker loop");
    if (!loop->accepting || loop->executor_stopping || p->closed)
        return fail(error, ROTMOS_STATE, "storage intake is closing");
    if (p->loop != NULL && (p->loop != loop || p->identity != loop->identity ||
                            p->process != (long)getpid()))
        return fail(error, ROTMOS_STATE,
                    "provider is bound to another loop/run");
    if (loop->storage_count >= ROTMOS_STORAGE_MAX_OPERATIONS)
        return fail(error, ROTMOS_LIMIT,
                    "loop storage operation budget is full");
    status = arguments_check(p, args, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_storage_operation_config_init(&defaults);
    if (config != NULL)
        defaults = *config;
    if (defaults.check_scope != NULL) {
        if (args->action != ROTMOS_STORAGE_DELETE &&
            args->action != ROTMOS_STORAGE_COPY &&
            args->action != ROTMOS_STORAGE_MOVE)
            return fail(error, ROTMOS_INVALID,
                        "scope checks apply to DELETE/COPY/MOVE");
        if (!(p->config.capabilities & ROTMOS_STORAGE_CAN_SCOPE_CHECK))
            return fail(error, ROTMOS_NOT_SUPPORTED,
                        "provider cannot atomically check mutation scope");
    }
    if (defaults.timeout_ms < 0)
        return fail(error, ROTMOS_INVALID,
                    "storage timeout must be nonnegative; use timeout_disabled "
                    "for absence");
    if (defaults.timeout_ms == 0)
        defaults.timeout_ms = 30000;
    op = calloc(1, sizeof(*op));
    if (op == NULL)
        return fail(error, ROTMOS_NOMEM, "allocating storage operation");
    op->references = 1;
    op->loop = loop;
    op->provider = p;
    provider_ref(p);
    op->config = defaults;
    status = copy_arguments(op, args, error);
    if (status != ROTMOS_OK) {
        operation_unref(op);
        return status;
    }
    status = rotmos_waker_new(loop, operation_ready, operation_unref, op,
                              &op->waker, error);
    if (status != ROTMOS_OK) {
        operation_unref(op);
        return status;
    }
    ++op->references;
    if (!defaults.timeout_disabled) {
        status = rotmos_loop_timer(loop, defaults.timeout_ms, 0, deadline,
                                   deadline_retired, op, &op->timer, error);
        if (status != ROTMOS_OK) {
            rotmos_waker_destroy(op->waker, NULL);
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
    rotmos_error_clear(&native);
    status = p->config.invoke(op, &op->arguments, p->config.userdata, &native);
    op->invoke_busy = 0;
    if (status == ROTMOS_OK && !op->acknowledged) {
        status = rotmos_storage_operation_complete(op, NULL, &native);
        if (status != ROTMOS_OK)
            protocol_failure(op, status, &native);
    } else if (status != ROTMOS_OK && status != ROTMOS_AGAIN)
        protocol_failure(op, status, &native);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
void rotmos_storage_stopping(rotmos_loop *loop)
{
    rotmos_storage_operation *op, *next;
    if (loop == NULL)
        return;
    for (op = loop->storage_operations; op != NULL; op = next) {
        next = op->next;
        (void)cancel_operation(op, ROTMOS_CANCELLED, 0, NULL);
    }
}
