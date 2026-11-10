#include "asset_internal.h"
#include <stdlib.h>
#include <string.h>

struct knurl_asset_provider {
    knurl_asset_provider_config config;
    size_t references;
    int closed, busy;
};
struct knurl_asset {
    knurl_asset_config config;
    knurl_asset_provider *provider;
    void *owner;
    knurl_asset_entry entry;
    knurl_status failure;
    int busy, ended;
    knurl_retire_fn retained_release;
    void *retained_data;
};
int knurl_asset_compare(knurl_u64 a, knurl_u64 b)
{
    return a.high != b.high ? (a.high < b.high ? -1 : 1)
           : a.low == b.low ? 0
           : a.low < b.low  ? -1
                            : 1;
}
knurl_u64 knurl_asset_subtract(knurl_u64 a, knurl_u64 b)
{
    knurl_u64 r;
    r.high = a.high - b.high - (a.low < b.low);
    r.low = (a.low - b.low) & 0xffffffffUL;
    return r;
}
int knurl_asset_path_valid(const char *path)
{
    size_t n, segment = 1;
    if (path == NULL || path[0] != '/')
        return 0;
    if (path[1] == 0)
        return 1;
    for (n = 1; n <= KNURL_ASSET_PATH_MAX; ++n) {
        unsigned char c = (unsigned char)path[n];
        if (c == '/' || c == 0) {
            size_t length = n - segment;
            if (length == 0 || (length == 1 && path[segment] == '.') ||
                (length == 2 && path[segment] == '.' &&
                 path[segment + 1] == '.'))
                return 0;
            if (c == 0)
                return 1;
            segment = n + 1;
        } else if (c < 32 || c == 127 || c == '\\' || c == '%' || c == '?')
            return 0;
    }
    return 0;
}
static knurl_status failure(knurl_error *error, knurl_status status,
                            const char *message)
{
    return knurl_fail(error, status, "asset", message);
}
static int bounded(const char *s, size_t bound)
{
    size_t n;
    if (s == NULL)
        return 1;
    for (n = 0; n <= bound; ++n)
        if (s[n] == 0)
            return 1;
    return 0;
}
int knurl_asset_child_valid(const char *name)
{
    size_t n;
    if (name == NULL || name[0] == 0 || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    for (n = 0; n <= KNURL_ASSET_PATH_MAX; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (c == 0)
            return 1;
        /* Listing names are literal directory components. URL punctuation is
         * encoded by the HTTP facade; it is never decoded as another path. */
        if (c < 32 || c == 127 || c == '/')
            return 0;
    }
    return 0;
}
static int tag(const char *s)
{
    if (s == NULL || s[0] == 0)
        return 1;
    if (s[0] == 'W' && s[1] == '/')
        s += 2;
    if (*s++ != '"')
        return 0;
    while (*s != '"') {
        if ((unsigned char)*s < 0x21 || (unsigned char)*s == 127)
            return 0;
        ++s;
    }
    return s[1] == 0;
}
void knurl_asset_info_clear(knurl_asset_info *info)
{
    free((void *)info->etag);
    free((void *)info->content_type);
    free((void *)info->last_modified);
    memset(info, 0, sizeof(*info));
}
knurl_status knurl_asset_info_copy(const knurl_asset_info *input,
                                   knurl_asset_info *out, knurl_error *error)
{
    memset(out, 0, sizeof(*out));
    if (input == NULL ||
        (input->kind != KNURL_ASSET_FILE &&
         input->kind != KNURL_ASSET_DIRECTORY) ||
        !knurl_u64_valid(input->size) ||
        !bounded(input->etag, KNURL_ASSET_ETAG_MAX) ||
        !bounded(input->content_type, KNURL_ASSET_TYPE_MAX) ||
        !bounded(input->last_modified, KNURL_ASSET_DATE_MAX) ||
        (input->last_modified != NULL &&
         (!knurl_header_value_valid(input->last_modified) ||
          input->last_modified[0] == 0)) ||
        !tag(input->etag) ||
        (input->content_type != NULL &&
         !knurl_content_type_valid(input->content_type)))
        return failure(error, KNURL_INVALID,
                       "invalid asset kind, size, ETag, content type or date");
    *out = *input;
    out->etag = NULL;
    out->content_type = NULL;
    out->last_modified = NULL;
    if (input->etag != NULL)
        out->etag = knurl_copy_text(input->etag);
    if (input->content_type != NULL)
        out->content_type = knurl_copy_text(input->content_type);
    if (input->last_modified != NULL)
        out->last_modified = knurl_copy_text(input->last_modified);
    if ((input->etag != NULL && out->etag == NULL) ||
        (input->content_type != NULL && out->content_type == NULL) ||
        (input->last_modified != NULL && out->last_modified == NULL)) {
        knurl_asset_info_clear(out);
        return failure(error, KNURL_NOMEM, "copying asset metadata");
    }
    return KNURL_OK;
}
void knurl_asset_config_init(knurl_asset_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
knurl_status knurl_asset_new(const knurl_asset_config *config,
                             knurl_asset **out, knurl_error *error)
{
    knurl_asset *a;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL ||
        (config->info.kind == KNURL_ASSET_FILE && config->read == NULL) ||
        (config->info.kind == KNURL_ASSET_DIRECTORY && config->next == NULL))
        return failure(error, KNURL_INVALID,
                       "provide asset metadata and read/list callback");
    a = calloc(1, sizeof(*a));
    if (a == NULL)
        return failure(error, KNURL_NOMEM, "allocating asset");
    status = knurl_asset_info_copy(&config->info, &a->config.info, error);
    if (status != KNURL_OK) {
        free(a);
        return status;
    }
    a->config.read = config->read;
    a->config.next = config->next;
    a->config.close = config->close;
    a->config.userdata = config->userdata;
    *out = a;
    knurl_error_clear(error);
    return KNURL_OK;
}
const knurl_asset_info *knurl_asset_stat(const knurl_asset *a)
{
    return a != NULL ? &a->config.info : NULL;
}
static knurl_status access(knurl_asset *a, void *owner, knurl_error *error)
{
    if (a == NULL)
        return failure(error, KNURL_INVALID, "missing asset");
    if (a->busy) {
        a->failure = KNURL_STATE;
        return failure(error, KNURL_STATE, "recursive asset operation");
    }
    if (a->owner != owner)
        return failure(error, KNURL_STATE, "asset belongs to a source");
    if (a->failure != KNURL_OK)
        return failure(error, a->failure, "asset operation previously failed");
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status read_at(knurl_asset *a, void *owner, knurl_u64 offset,
                            void *bytes, size_t capacity, size_t *produced,
                            knurl_error *error)
{
    knurl_u64 remaining;
    size_t count = 0, bound;
    knurl_status status;
    if (produced != NULL)
        *produced = 0;
    status = access(a, owner, error);
    if (status != KNURL_OK)
        return status;
    if (a->config.info.kind != KNURL_ASSET_FILE || !knurl_u64_valid(offset) ||
        bytes == NULL || capacity == 0 || produced == NULL)
        return failure(
            error, KNURL_INVALID,
            "provide file asset, valid offset and nonempty read buffer");
    if (knurl_asset_compare(offset, a->config.info.size) >= 0)
        return KNURL_EOF;
    remaining = knurl_asset_subtract(a->config.info.size, offset);
    if (knurl_u64_to_size(remaining, &bound, NULL) == KNURL_OK &&
        capacity > bound)
        capacity = bound;
    a->busy = 1;
    status = a->config.read(offset, bytes, capacity, &count, a->config.userdata,
                            error);
    a->busy = 0;
    if (a->failure != KNURL_OK)
        status = a->failure;
    else if (status < KNURL_OK || status > KNURL_BACKEND ||
             (status == KNURL_OK && (count == 0 || count > capacity)) ||
             (status != KNURL_OK && count != 0))
        status = KNURL_INVALID;
    else if (status == KNURL_EOF)
        status = KNURL_IO;
    if (status != KNURL_OK && status != KNURL_AGAIN) {
        a->failure = status;
        return error != NULL && error->status == status
                   ? status
                   : failure(error, status,
                             "asset read failed or returned an invalid count");
    }
    *produced = count;
    knurl_error_clear(error);
    return status;
}
knurl_status knurl_asset_read(knurl_asset *a, knurl_u64 offset, void *bytes,
                              size_t capacity, size_t *produced,
                              knurl_error *error)
{
    return read_at(a, NULL, offset, bytes, capacity, produced, error);
}
knurl_status knurl_asset_list_next(knurl_asset *a, knurl_asset_entry *out,
                                   knurl_error *error)
{
    knurl_asset_entry entry;
    knurl_status status;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    status = access(a, NULL, error);
    if (status != KNURL_OK)
        return status;
    if (out == NULL || a->config.info.kind != KNURL_ASSET_DIRECTORY)
        return failure(error, KNURL_INVALID,
                       "provide directory asset and entry output");
    if (a->ended)
        return KNURL_EOF;
    free((void *)a->entry.name);
    knurl_asset_info_clear(&a->entry.info);
    memset(&a->entry, 0, sizeof(a->entry));
    memset(&entry, 0, sizeof(entry));
    a->busy = 1;
    status = a->config.next(&entry, a->config.userdata, error);
    a->busy = 0;
    if (a->failure != KNURL_OK)
        status = a->failure;
    if (status == KNURL_EOF) {
        a->ended = 1;
        knurl_error_clear(error);
        return status;
    }
    if (status == KNURL_AGAIN) {
        knurl_error_clear(error);
        return status;
    }
    if (status == KNURL_OK) {
        if (!knurl_asset_child_valid(entry.name))
            status =
                failure(error, KNURL_INVALID, "invalid asset list child name");
        else
            status = knurl_asset_info_copy(&entry.info, &a->entry.info, error);
        if (status == KNURL_OK) {
            a->entry.name = knurl_copy_text(entry.name);
            if (a->entry.name == NULL)
                status = failure(error, KNURL_NOMEM, "copying list child name");
        }
    }
    if (status != KNURL_OK) {
        if (status < KNURL_OK || status > KNURL_BACKEND)
            status = KNURL_INVALID;
        a->failure = status;
        return error != NULL && error->status == status
                   ? status
                   : failure(error, status, "asset list callback failed");
    }
    *out = a->entry;
    knurl_error_clear(error);
    return KNURL_OK;
}
static void provider_unref(knurl_asset_provider *p)
{
    if (p != NULL && --p->references == 0) {
        p->busy = 1;
        if (p->config.close != NULL)
            p->config.close(p->config.userdata);
        free(p);
    }
}
knurl_status knurl_asset_close(knurl_asset *a, knurl_error *error)
{
    if (a == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (a->busy) {
        a->failure = KNURL_STATE;
        return failure(error, KNURL_STATE, "recursive asset close");
    }
    if (a->owner != NULL)
        return failure(error, KNURL_STATE, "asset belongs to a source");
    a->busy = 1;
    if (a->config.close != NULL)
        a->config.close(a->config.userdata);
    knurl_asset_info_clear(&a->config.info);
    knurl_asset_info_clear(&a->entry.info);
    free((void *)a->entry.name);
    provider_unref(a->provider);
    if (a->retained_release != NULL)
        a->retained_release(a->retained_data);
    free(a);
    knurl_error_clear(error);
    return KNURL_OK;
}
struct asset_source {
    knurl_asset *asset;
    knurl_u64 offset, remaining, start, length;
};
static knurl_status source_read(void *bytes, size_t capacity, size_t *produced,
                                void *data, knurl_error *error)
{
    struct asset_source *s = data;
    knurl_status status;
    knurl_u64 amount, next;
    size_t bound;
    *produced = 0;
    if (s->remaining.high == 0 && s->remaining.low == 0)
        return KNURL_EOF;
    if (knurl_u64_to_size(s->remaining, &bound, NULL) == KNURL_OK &&
        capacity > bound)
        capacity = bound;
    status = read_at(s->asset, s, s->offset, bytes, capacity, produced, error);
    if (status != KNURL_OK)
        return status;
    knurl_u64_from_size(*produced, &amount, NULL);
    status = knurl_u64_add(s->offset, amount, &next, error);
    if (status != KNURL_OK) {
        *produced = 0;
        return status;
    }
    s->offset = next;
    s->remaining = knurl_asset_subtract(s->remaining, amount);
    return KNURL_OK;
}
static void source_close(void *data, knurl_status outcome)
{
    struct asset_source *s = data;
    (void)outcome;
    s->asset->owner = NULL;
    knurl_asset_close(s->asset, NULL);
    free(s);
}
static knurl_status source_reset(void *data, knurl_error *error)
{
    struct asset_source *s = data;
    if (s->asset->failure != KNURL_OK)
        return failure(error, s->asset->failure,
                       "asset read previously failed");
    s->offset = s->start;
    s->remaining = s->length;
    return KNURL_OK;
}
knurl_status knurl_asset_source_take(knurl_asset *a, knurl_u64 offset,
                                     knurl_u64 length, knurl_source **out,
                                     knurl_error *error)
{
    struct asset_source *s;
    knurl_source_config config;
    knurl_status status;
    knurl_u64 end;
    if (out != NULL)
        *out = NULL;
    status = access(a, NULL, error);
    if (status != KNURL_OK)
        return status;
    if (out == NULL || a->config.info.kind != KNURL_ASSET_FILE)
        return failure(error, KNURL_INVALID,
                       "provide file asset and source output");
    status = knurl_u64_add(offset, length, &end, error);
    if (status != KNURL_OK)
        return status;
    if (knurl_asset_compare(end, a->config.info.size) > 0)
        return failure(error, KNURL_INVALID,
                       "asset source range exceeds the opened content");
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return failure(error, KNURL_NOMEM, "allocating asset source");
    s->asset = a;
    s->offset = offset;
    s->remaining = length;
    s->start = offset;
    s->length = length;
    knurl_source_config_init(&config);
    config.read = source_read;
    config.reset = source_reset;
    config.close = source_close;
    config.userdata = s;
    config.has_length = 1;
    config.length = length;
    status = knurl_source_new(&config, out, error);
    if (status != KNURL_OK) {
        free(s);
        return status;
    }
    a->owner = s;
    return KNURL_OK;
}
struct memory {
    knurl_bytes bytes;
    knurl_retire_fn close;
    void *userdata;
};
static knurl_status memory_read(knurl_u64 offset, void *bytes, size_t capacity,
                                size_t *produced, void *data,
                                knurl_error *error)
{
    struct memory *s = data;
    size_t start;
    knurl_status status = knurl_u64_to_size(offset, &start, error);
    if (status != KNURL_OK)
        return status;
    if (start >= s->bytes.length) {
        *produced = 0;
        return KNURL_EOF;
    }
    *produced = s->bytes.length - start;
    if (*produced > capacity)
        *produced = capacity;
    memcpy(bytes, (const char *)s->bytes.data + start, *produced);
    return KNURL_OK;
}
static void memory_close(void *data)
{
    struct memory *s = data;
    if (s->close != NULL)
        s->close(s->userdata);
    free(s);
}
knurl_status knurl_asset_memory(const knurl_asset_info *info, knurl_bytes bytes,
                                knurl_retire_fn close, void *userdata,
                                knurl_asset **out, knurl_error *error)
{
    struct memory *s;
    knurl_asset_config config;
    knurl_status status;
    knurl_u64 size;
    if (out != NULL)
        *out = NULL;
    if (info == NULL || info->kind != KNURL_ASSET_FILE ||
        (bytes.data == NULL && bytes.length != 0))
        return failure(error, KNURL_INVALID,
                       "provide file metadata and borrowed byte span");
    knurl_u64_from_size(bytes.length, &size, NULL);
    if (knurl_asset_compare(info->size, size) != 0)
        return failure(error, KNURL_INVALID,
                       "asset metadata size differs from borrowed bytes");
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return failure(error, KNURL_NOMEM, "allocating borrowed memory asset");
    s->bytes = bytes;
    knurl_asset_config_init(&config);
    config.info = *info;
    config.read = memory_read;
    config.close = memory_close;
    config.userdata = s;
    status = knurl_asset_new(&config, out, error);
    if (status != KNURL_OK)
        free(s);
    else {
        s->close = close;
        s->userdata = userdata;
    }
    return status;
}
void knurl_asset_provider_config_init(knurl_asset_provider_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
knurl_status knurl_asset_provider_new(const knurl_asset_provider_config *config,
                                      knurl_asset_provider **out,
                                      knurl_error *error)
{
    knurl_asset_provider *p;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->open == NULL)
        return failure(error, KNURL_INVALID,
                       "provide asset open callback and provider output");
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return failure(error, KNURL_NOMEM, "allocating asset provider");
    p->config = *config;
    p->references = 1;
    *out = p;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_asset_provider_open(knurl_asset_provider *p,
                                       const char *path, knurl_asset **out,
                                       knurl_error *error)
{
    knurl_status status;
    knurl_asset *a = NULL;
    if (out != NULL)
        *out = NULL;
    if (p == NULL || out == NULL || !knurl_asset_path_valid(path))
        return failure(
            error, KNURL_INVALID,
            "provide provider, normalized absolute asset path and output");
    if (p->closed || p->busy)
        return failure(error, KNURL_STATE, "asset provider is closed or busy");
    p->busy = 1;
    knurl_error_clear(error);
    status = p->config.open(path, &a, p->config.userdata, error);
    p->busy = 0;
    if (status == KNURL_OK && a != NULL && a->provider == NULL &&
        a->owner == NULL && !a->busy) {
        a->provider = p;
        ++p->references;
        *out = a;
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (a != NULL && a->provider == NULL && a->owner == NULL)
        knurl_asset_close(a, NULL);
    if (status == KNURL_EOF && a == NULL) {
        knurl_error_clear(error);
        return status;
    }
    if (status == KNURL_OK || status == KNURL_AGAIN || status < KNURL_OK ||
        status > KNURL_BACKEND)
        status = KNURL_INVALID;
    return error != NULL && error->status == status
               ? status
               : failure(error, status,
                         "asset provider must synchronously return one new "
                         "asset or EOF");
}
knurl_status knurl_asset_provider_close(knurl_asset_provider *p,
                                        knurl_error *error)
{
    if (p == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (p->busy || p->closed)
        return failure(error, KNURL_STATE, "asset provider is closed or busy");
    p->closed = 1;
    provider_unref(p);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_asset_claim(knurl_asset *asset, void *owner,
                               knurl_retire_fn release, void *data,
                               knurl_error *error)
{
    knurl_status status = access(asset, NULL, error);
    if (status != KNURL_OK)
        return status;
    if (asset->retained_release != NULL)
        return failure(error, KNURL_STATE,
                       "storage result must be a new unclaimed asset");
    asset->owner = owner;
    asset->retained_release = release;
    asset->retained_data = data;
    return KNURL_OK;
}
void knurl_asset_unclaim(knurl_asset *asset, void *owner)
{
    if (asset != NULL && asset->owner == owner)
        asset->owner = NULL;
}
