#include <rotmos/storage.h>
#include <rotmos/request.h>
#include "internal.h"
#include <stdlib.h>

struct list_retry {
    rotmos_storage_operation *operation;
    rotmos_timer *timer;
    size_t references;
};
static void close_directory(void *data) { rotmos_asset_close(data, NULL); }
static rotmos_status stage_list(rotmos_storage_operation *op,
                                rotmos_error *error)
{
    const rotmos_storage_arguments *args =
        rotmos_storage_operation_arguments(op);
    rotmos_asset *directory = rotmos_storage_resource_userdata(args->resource);
    rotmos_asset_entry entry;
    rotmos_storage_result result;
    rotmos_status status;
    rotmos_storage_result_init(&result);
    status = rotmos_asset_list_next(directory, &entry, error);
    if (status == ROTMOS_AGAIN)
        return status;
    if (status == ROTMOS_EOF)
        result.code = ROTMOS_STORAGE_END;
    else if (status == ROTMOS_OK) {
        result.has_entry = 1;
        result.entry = entry;
    } else
        return status;
    return rotmos_storage_operation_complete(op, &result, error);
}
static void retry_unref(struct list_retry *retry)
{
    if (--retry->references == 0)
        free(retry);
}
static void retry_retired(void *data)
{
    struct list_retry *retry = data;
    retry->timer = NULL;
    retry_unref(retry);
}
static void retry_list(rotmos_timer *timer, void *data)
{
    struct list_retry *retry = data;
    rotmos_error error;
    rotmos_storage_result result;
    rotmos_status status;
    if (retry->operation == NULL) {
        rotmos_timer_cancel(timer);
        return;
    }
    status = stage_list(retry->operation, &error);
    if (status == ROTMOS_AGAIN)
        return;
    if (status != ROTMOS_OK) {
        rotmos_storage_result_init(&result);
        result.code = ROTMOS_STORAGE_IO;
        result.error = error;
        (void)rotmos_storage_operation_complete(retry->operation, &result,
                                                NULL);
    }
    rotmos_timer_cancel(timer);
    /* Provider retirement clears the operation pointer before timer cleanup. */
}
static rotmos_status invoke(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *args, void *data,
                            rotmos_error *error)
{
    rotmos_asset_provider *provider = data;
    rotmos_asset *asset = NULL;
    rotmos_storage_result result;
    rotmos_storage_resource_config config;
    rotmos_storage_resource *resource;
    rotmos_status status;
    struct list_retry *retry;
    rotmos_storage_result_init(&result);
    if (args->action == ROTMOS_STORAGE_LIST_NEXT) {
        status = stage_list(op, error);
        if (status != ROTMOS_AGAIN)
            return status;
        retry = calloc(1, sizeof(*retry));
        if (retry == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "storage_assets",
                               "allocating bounded listing retry");
        retry->operation = op;
        retry->references = 1;
        status = rotmos_storage_operation_set_userdata(op, retry, error);
        if (status != ROTMOS_OK) {
            free(retry);
            return status;
        }
        /* The immutable resource retains its provider; retry work stays on the
         * same owner loop. No directory tree or callback pointer is collected.
         */
        status = rotmos_loop_timer(rotmos_storage_operation_loop(op), 1, 1,
                                   retry_list, retry_retired, retry,
                                   &retry->timer, error);
        if (status != ROTMOS_OK) {
            retry->timer = NULL;
            return status;
        }
        ++retry->references;
        return ROTMOS_AGAIN;
    }
    status = rotmos_asset_provider_open(provider, args->path, &asset, error);
    if (status == ROTMOS_EOF) {
        result.code = ROTMOS_STORAGE_NOT_FOUND;
        return rotmos_storage_operation_complete(op, &result, error);
    }
    if (status != ROTMOS_OK)
        return status;
    if (args->action == ROTMOS_STORAGE_STAT) {
        result.has_info = 1;
        result.info = *rotmos_asset_stat(asset);
        status = rotmos_storage_operation_complete(op, &result, error);
        rotmos_asset_close(asset, NULL);
        return status;
    }
    if (args->action == ROTMOS_STORAGE_OPEN_READ) {
        if (rotmos_asset_stat(asset)->kind != ROTMOS_ASSET_FILE) {
            rotmos_asset_close(asset, NULL);
            result.code = ROTMOS_STORAGE_CONFLICT;
            return rotmos_storage_operation_complete(op, &result, error);
        }
        result.asset = asset;
        status = rotmos_storage_operation_complete(op, &result, error);
        if (status != ROTMOS_OK)
            rotmos_asset_close(asset, NULL);
        return status;
    }
    if (args->action == ROTMOS_STORAGE_LIST_OPEN) {
        if (rotmos_asset_stat(asset)->kind != ROTMOS_ASSET_DIRECTORY) {
            rotmos_asset_close(asset, NULL);
            result.code = ROTMOS_STORAGE_CONFLICT;
            return rotmos_storage_operation_complete(op, &result, error);
        }
        rotmos_storage_resource_config_init(&config);
        config.kind = ROTMOS_STORAGE_LIST;
        config.userdata = asset;
        config.close = close_directory;
        status = rotmos_storage_resource_new(op, &config, &resource, error);
        if (status != ROTMOS_OK) {
            rotmos_asset_close(asset, NULL);
            return status;
        }
        result.resource = resource;
        result.has_info = 1;
        result.info = *rotmos_asset_stat(asset);
        status = rotmos_storage_operation_complete(op, &result, error);
        if (status != ROTMOS_OK)
            rotmos_storage_resource_close(resource, NULL);
        return status;
    }
    rotmos_asset_close(asset, NULL);
    return ROTMOS_NOT_SUPPORTED;
}
static rotmos_status cancel(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *args, void *data,
                            rotmos_error *error)
{
    struct list_retry *retry = rotmos_storage_operation_userdata(op);
    (void)args;
    (void)data;
    if (retry != NULL && retry->timer != NULL)
        rotmos_timer_cancel(retry->timer);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static void retired(rotmos_storage_operation *op,
                    const rotmos_storage_outcome *outcome, void *data)
{
    struct list_retry *retry = rotmos_storage_operation_userdata(op);
    (void)outcome;
    (void)data;
    if (retry != NULL) {
        retry->operation = NULL;
        if (retry->timer != NULL)
            rotmos_timer_cancel(retry->timer);
        retry_unref(retry);
    }
}
rotmos_status rotmos_storage_provider_assets(rotmos_asset_provider *provider,
                                             rotmos_storage_provider **out,
                                             rotmos_error *error)
{
    rotmos_storage_provider_config config;
    if (provider == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "storage_assets",
                           "provide a borrowed asset provider");
    rotmos_storage_provider_config_init(&config);
    config.invoke = invoke;
    config.cancel = cancel;
    config.retired = retired;
    config.userdata = provider;
    return rotmos_storage_provider_new(&config, out, error);
}
