#include <knurl/storage.h>
#include <knurl/request.h>
#include "internal.h"
#include <stdlib.h>

struct list_retry {
    knurl_storage_operation *operation;
    knurl_timer *timer;
    size_t references;
};
static void close_directory(void *data) { knurl_asset_close(data, NULL); }
static knurl_status stage_list(knurl_storage_operation *op, knurl_error *error)
{
    const knurl_storage_arguments *args = knurl_storage_operation_arguments(op);
    knurl_asset *directory = knurl_storage_resource_userdata(args->resource);
    knurl_asset_entry entry;
    knurl_storage_result result;
    knurl_status status;
    knurl_storage_result_init(&result);
    status = knurl_asset_list_next(directory, &entry, error);
    if (status == KNURL_AGAIN)
        return status;
    if (status == KNURL_EOF)
        result.code = KNURL_STORAGE_END;
    else if (status == KNURL_OK) {
        result.has_entry = 1;
        result.entry = entry;
    } else
        return status;
    return knurl_storage_operation_complete(op, &result, error);
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
static void retry_list(knurl_timer *timer, void *data)
{
    struct list_retry *retry = data;
    knurl_error error;
    knurl_storage_result result;
    knurl_status status;
    if (retry->operation == NULL) {
        knurl_timer_cancel(timer);
        return;
    }
    status = stage_list(retry->operation, &error);
    if (status == KNURL_AGAIN)
        return;
    if (status != KNURL_OK) {
        knurl_storage_result_init(&result);
        result.code = KNURL_STORAGE_IO;
        result.error = error;
        (void)knurl_storage_operation_complete(retry->operation, &result, NULL);
    }
    knurl_timer_cancel(timer);
    /* Provider retirement clears the operation pointer before timer cleanup. */
}
static knurl_status invoke(knurl_storage_operation *op,
                           const knurl_storage_arguments *args, void *data,
                           knurl_error *error)
{
    knurl_asset_provider *provider = data;
    knurl_asset *asset = NULL;
    knurl_storage_result result;
    knurl_storage_resource_config config;
    knurl_storage_resource *resource;
    knurl_status status;
    struct list_retry *retry;
    knurl_storage_result_init(&result);
    if (args->action == KNURL_STORAGE_LIST_NEXT) {
        status = stage_list(op, error);
        if (status != KNURL_AGAIN)
            return status;
        retry = calloc(1, sizeof(*retry));
        if (retry == NULL)
            return knurl_fail(error, KNURL_NOMEM, "storage_assets",
                              "allocating bounded listing retry");
        retry->operation = op;
        retry->references = 1;
        status = knurl_storage_operation_set_userdata(op, retry, error);
        if (status != KNURL_OK) {
            free(retry);
            return status;
        }
        /* The immutable resource retains its provider; retry work stays on the
         * same owner loop. No directory tree or callback pointer is collected.
         */
        status =
            knurl_loop_timer(knurl_storage_operation_loop(op), 1, 1, retry_list,
                             retry_retired, retry, &retry->timer, error);
        if (status != KNURL_OK) {
            retry->timer = NULL;
            return status;
        }
        ++retry->references;
        return KNURL_AGAIN;
    }
    status = knurl_asset_provider_open(provider, args->path, &asset, error);
    if (status == KNURL_EOF) {
        result.code = KNURL_STORAGE_NOT_FOUND;
        return knurl_storage_operation_complete(op, &result, error);
    }
    if (status != KNURL_OK)
        return status;
    if (args->action == KNURL_STORAGE_STAT) {
        result.has_info = 1;
        result.info = *knurl_asset_stat(asset);
        status = knurl_storage_operation_complete(op, &result, error);
        knurl_asset_close(asset, NULL);
        return status;
    }
    if (args->action == KNURL_STORAGE_OPEN_READ) {
        if (knurl_asset_stat(asset)->kind != KNURL_ASSET_FILE) {
            knurl_asset_close(asset, NULL);
            result.code = KNURL_STORAGE_CONFLICT;
            return knurl_storage_operation_complete(op, &result, error);
        }
        result.asset = asset;
        status = knurl_storage_operation_complete(op, &result, error);
        if (status != KNURL_OK)
            knurl_asset_close(asset, NULL);
        return status;
    }
    if (args->action == KNURL_STORAGE_LIST_OPEN) {
        if (knurl_asset_stat(asset)->kind != KNURL_ASSET_DIRECTORY) {
            knurl_asset_close(asset, NULL);
            result.code = KNURL_STORAGE_CONFLICT;
            return knurl_storage_operation_complete(op, &result, error);
        }
        knurl_storage_resource_config_init(&config);
        config.kind = KNURL_STORAGE_LIST;
        config.userdata = asset;
        config.close = close_directory;
        status = knurl_storage_resource_new(op, &config, &resource, error);
        if (status != KNURL_OK) {
            knurl_asset_close(asset, NULL);
            return status;
        }
        result.resource = resource;
        result.has_info = 1;
        result.info = *knurl_asset_stat(asset);
        status = knurl_storage_operation_complete(op, &result, error);
        if (status != KNURL_OK)
            knurl_storage_resource_close(resource, NULL);
        return status;
    }
    knurl_asset_close(asset, NULL);
    return KNURL_NOT_SUPPORTED;
}
static knurl_status cancel(knurl_storage_operation *op,
                           const knurl_storage_arguments *args, void *data,
                           knurl_error *error)
{
    struct list_retry *retry = knurl_storage_operation_userdata(op);
    (void)args;
    (void)data;
    if (retry != NULL && retry->timer != NULL)
        knurl_timer_cancel(retry->timer);
    knurl_error_clear(error);
    return KNURL_OK;
}
static void retired(knurl_storage_operation *op,
                    const knurl_storage_outcome *outcome, void *data)
{
    struct list_retry *retry = knurl_storage_operation_userdata(op);
    (void)outcome;
    (void)data;
    if (retry != NULL) {
        retry->operation = NULL;
        if (retry->timer != NULL)
            knurl_timer_cancel(retry->timer);
        retry_unref(retry);
    }
}
knurl_status knurl_storage_provider_assets(knurl_asset_provider *provider,
                                           knurl_storage_provider **out,
                                           knurl_error *error)
{
    knurl_storage_provider_config config;
    if (provider == NULL)
        return knurl_fail(error, KNURL_INVALID, "storage_assets",
                          "provide a borrowed asset provider");
    knurl_storage_provider_config_init(&config);
    config.invoke = invoke;
    config.cancel = cancel;
    config.retired = retired;
    config.userdata = provider;
    return knurl_storage_provider_new(&config, out, error);
}
