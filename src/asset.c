#include "asset_internal.h"
#include <stdlib.h>
#include <string.h>

struct rotmos_asset_provider {
    rotmos_asset_provider_config config;
    size_t references;
    int closed, busy;
};
struct rotmos_asset {
    rotmos_asset_config config;
    rotmos_asset_provider *provider;
    void *owner;
    rotmos_asset_entry entry;
    rotmos_status failure;
    int busy, ended;
    rotmos_retire_fn retained_release;
    void *retained_data;
};
int rotmos_asset_compare(rotmos_u64 a, rotmos_u64 b)
{
    return a.high != b.high ? (a.high < b.high ? -1 : 1)
           : a.low == b.low ? 0
           : a.low < b.low  ? -1
                            : 1;
}
rotmos_u64 rotmos_asset_subtract(rotmos_u64 a, rotmos_u64 b)
{
    rotmos_u64 r;
    r.high = a.high - b.high - (a.low < b.low);
    r.low = (a.low - b.low) & 0xffffffffUL;
    return r;
}
int rotmos_asset_path_valid(const char *path)
{
    size_t n, segment = 1;
    if (path == NULL || path[0] != '/')
        return 0;
    if (path[1] == 0)
        return 1;
    for (n = 1; n <= ROTMOS_ASSET_PATH_MAX; ++n) {
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
static rotmos_status failure(rotmos_error *error, rotmos_status status,
                             const char *message)
{
    return rotmos_fail(error, status, "asset", message);
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
int rotmos_asset_child_valid(const char *name)
{
    size_t n;
    if (name == NULL || name[0] == 0 || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    for (n = 0; n <= ROTMOS_ASSET_PATH_MAX; ++n) {
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
void rotmos_asset_info_clear(rotmos_asset_info *info)
{
    free((void *)info->etag);
    free((void *)info->content_type);
    free((void *)info->last_modified);
    memset(info, 0, sizeof(*info));
}
rotmos_status rotmos_asset_info_copy(const rotmos_asset_info *input,
                                     rotmos_asset_info *out,
                                     rotmos_error *error)
{
    memset(out, 0, sizeof(*out));
    if (input == NULL ||
        (input->kind != ROTMOS_ASSET_FILE &&
         input->kind != ROTMOS_ASSET_DIRECTORY) ||
        !rotmos_u64_valid(input->size) ||
        !bounded(input->etag, ROTMOS_ASSET_ETAG_MAX) ||
        !bounded(input->content_type, ROTMOS_ASSET_TYPE_MAX) ||
        !bounded(input->last_modified, ROTMOS_ASSET_DATE_MAX) ||
        (input->last_modified != NULL &&
         (!rotmos_header_value_valid(input->last_modified) ||
          input->last_modified[0] == 0)) ||
        !tag(input->etag) ||
        (input->content_type != NULL &&
         !rotmos_content_type_valid(input->content_type)))
        return failure(error, ROTMOS_INVALID,
                       "invalid asset kind, size, ETag, content type or date");
    *out = *input;
    out->etag = NULL;
    out->content_type = NULL;
    out->last_modified = NULL;
    if (input->etag != NULL)
        out->etag = rotmos_copy_text(input->etag);
    if (input->content_type != NULL)
        out->content_type = rotmos_copy_text(input->content_type);
    if (input->last_modified != NULL)
        out->last_modified = rotmos_copy_text(input->last_modified);
    if ((input->etag != NULL && out->etag == NULL) ||
        (input->content_type != NULL && out->content_type == NULL) ||
        (input->last_modified != NULL && out->last_modified == NULL)) {
        rotmos_asset_info_clear(out);
        return failure(error, ROTMOS_NOMEM, "copying asset metadata");
    }
    return ROTMOS_OK;
}
void rotmos_asset_config_init(rotmos_asset_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
rotmos_status rotmos_asset_new(const rotmos_asset_config *config,
                               rotmos_asset **out, rotmos_error *error)
{
    rotmos_asset *a;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL ||
        (config->info.kind == ROTMOS_ASSET_FILE && config->read == NULL) ||
        (config->info.kind == ROTMOS_ASSET_DIRECTORY && config->next == NULL))
        return failure(error, ROTMOS_INVALID,
                       "provide asset metadata and read/list callback");
    a = calloc(1, sizeof(*a));
    if (a == NULL)
        return failure(error, ROTMOS_NOMEM, "allocating asset");
    status = rotmos_asset_info_copy(&config->info, &a->config.info, error);
    if (status != ROTMOS_OK) {
        free(a);
        return status;
    }
    a->config.read = config->read;
    a->config.next = config->next;
    a->config.close = config->close;
    a->config.userdata = config->userdata;
    *out = a;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
const rotmos_asset_info *rotmos_asset_stat(const rotmos_asset *a)
{
    return a != NULL ? &a->config.info : NULL;
}
static rotmos_status access(rotmos_asset *a, void *owner, rotmos_error *error)
{
    if (a == NULL)
        return failure(error, ROTMOS_INVALID, "missing asset");
    if (a->busy) {
        a->failure = ROTMOS_STATE;
        return failure(error, ROTMOS_STATE, "recursive asset operation");
    }
    if (a->owner != owner)
        return failure(error, ROTMOS_STATE, "asset belongs to a source");
    if (a->failure != ROTMOS_OK)
        return failure(error, a->failure, "asset operation previously failed");
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static rotmos_status read_at(rotmos_asset *a, void *owner, rotmos_u64 offset,
                             void *bytes, size_t capacity, size_t *produced,
                             rotmos_error *error)
{
    rotmos_u64 remaining;
    size_t count = 0, bound;
    rotmos_status status;
    if (produced != NULL)
        *produced = 0;
    status = access(a, owner, error);
    if (status != ROTMOS_OK)
        return status;
    if (a->config.info.kind != ROTMOS_ASSET_FILE || !rotmos_u64_valid(offset) ||
        bytes == NULL || capacity == 0 || produced == NULL)
        return failure(
            error, ROTMOS_INVALID,
            "provide file asset, valid offset and nonempty read buffer");
    if (rotmos_asset_compare(offset, a->config.info.size) >= 0)
        return ROTMOS_EOF;
    remaining = rotmos_asset_subtract(a->config.info.size, offset);
    if (rotmos_u64_to_size(remaining, &bound, NULL) == ROTMOS_OK &&
        capacity > bound)
        capacity = bound;
    a->busy = 1;
    status = a->config.read(offset, bytes, capacity, &count, a->config.userdata,
                            error);
    a->busy = 0;
    if (a->failure != ROTMOS_OK)
        status = a->failure;
    else if (status < ROTMOS_OK || status > ROTMOS_BACKEND ||
             (status == ROTMOS_OK && (count == 0 || count > capacity)) ||
             (status != ROTMOS_OK && count != 0))
        status = ROTMOS_INVALID;
    else if (status == ROTMOS_EOF)
        status = ROTMOS_IO;
    if (status != ROTMOS_OK && status != ROTMOS_AGAIN) {
        a->failure = status;
        return error != NULL && error->status == status
                   ? status
                   : failure(error, status,
                             "asset read failed or returned an invalid count");
    }
    *produced = count;
    rotmos_error_clear(error);
    return status;
}
rotmos_status rotmos_asset_read(rotmos_asset *a, rotmos_u64 offset, void *bytes,
                                size_t capacity, size_t *produced,
                                rotmos_error *error)
{
    return read_at(a, NULL, offset, bytes, capacity, produced, error);
}
rotmos_status rotmos_asset_list_next(rotmos_asset *a, rotmos_asset_entry *out,
                                     rotmos_error *error)
{
    rotmos_asset_entry entry;
    rotmos_status status;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    status = access(a, NULL, error);
    if (status != ROTMOS_OK)
        return status;
    if (out == NULL || a->config.info.kind != ROTMOS_ASSET_DIRECTORY)
        return failure(error, ROTMOS_INVALID,
                       "provide directory asset and entry output");
    if (a->ended)
        return ROTMOS_EOF;
    free((void *)a->entry.name);
    rotmos_asset_info_clear(&a->entry.info);
    memset(&a->entry, 0, sizeof(a->entry));
    memset(&entry, 0, sizeof(entry));
    a->busy = 1;
    status = a->config.next(&entry, a->config.userdata, error);
    a->busy = 0;
    if (a->failure != ROTMOS_OK)
        status = a->failure;
    if (status == ROTMOS_EOF) {
        a->ended = 1;
        rotmos_error_clear(error);
        return status;
    }
    if (status == ROTMOS_AGAIN) {
        rotmos_error_clear(error);
        return status;
    }
    if (status == ROTMOS_OK) {
        if (!rotmos_asset_child_valid(entry.name))
            status =
                failure(error, ROTMOS_INVALID, "invalid asset list child name");
        else
            status = rotmos_asset_info_copy(&entry.info, &a->entry.info, error);
        if (status == ROTMOS_OK) {
            a->entry.name = rotmos_copy_text(entry.name);
            if (a->entry.name == NULL)
                status =
                    failure(error, ROTMOS_NOMEM, "copying list child name");
        }
    }
    if (status != ROTMOS_OK) {
        if (status < ROTMOS_OK || status > ROTMOS_BACKEND)
            status = ROTMOS_INVALID;
        a->failure = status;
        return error != NULL && error->status == status
                   ? status
                   : failure(error, status, "asset list callback failed");
    }
    *out = a->entry;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static void provider_unref(rotmos_asset_provider *p)
{
    if (p != NULL && --p->references == 0) {
        p->busy = 1;
        if (p->config.close != NULL)
            p->config.close(p->config.userdata);
        free(p);
    }
}
rotmos_status rotmos_asset_close(rotmos_asset *a, rotmos_error *error)
{
    if (a == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (a->busy) {
        a->failure = ROTMOS_STATE;
        return failure(error, ROTMOS_STATE, "recursive asset close");
    }
    if (a->owner != NULL)
        return failure(error, ROTMOS_STATE, "asset belongs to a source");
    a->busy = 1;
    if (a->config.close != NULL)
        a->config.close(a->config.userdata);
    rotmos_asset_info_clear(&a->config.info);
    rotmos_asset_info_clear(&a->entry.info);
    free((void *)a->entry.name);
    provider_unref(a->provider);
    if (a->retained_release != NULL)
        a->retained_release(a->retained_data);
    free(a);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
struct asset_source {
    rotmos_asset *asset;
    rotmos_u64 offset, remaining, start, length;
};
static rotmos_status source_read(void *bytes, size_t capacity, size_t *produced,
                                 void *data, rotmos_error *error)
{
    struct asset_source *s = data;
    rotmos_status status;
    rotmos_u64 amount, next;
    size_t bound;
    *produced = 0;
    if (s->remaining.high == 0 && s->remaining.low == 0)
        return ROTMOS_EOF;
    if (rotmos_u64_to_size(s->remaining, &bound, NULL) == ROTMOS_OK &&
        capacity > bound)
        capacity = bound;
    status = read_at(s->asset, s, s->offset, bytes, capacity, produced, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_u64_from_size(*produced, &amount, NULL);
    status = rotmos_u64_add(s->offset, amount, &next, error);
    if (status != ROTMOS_OK) {
        *produced = 0;
        return status;
    }
    s->offset = next;
    s->remaining = rotmos_asset_subtract(s->remaining, amount);
    return ROTMOS_OK;
}
static void source_close(void *data, rotmos_status outcome)
{
    struct asset_source *s = data;
    (void)outcome;
    s->asset->owner = NULL;
    rotmos_asset_close(s->asset, NULL);
    free(s);
}
static rotmos_status source_reset(void *data, rotmos_error *error)
{
    struct asset_source *s = data;
    if (s->asset->failure != ROTMOS_OK)
        return failure(error, s->asset->failure,
                       "asset read previously failed");
    s->offset = s->start;
    s->remaining = s->length;
    return ROTMOS_OK;
}
rotmos_status rotmos_asset_source_take(rotmos_asset *a, rotmos_u64 offset,
                                       rotmos_u64 length, rotmos_source **out,
                                       rotmos_error *error)
{
    struct asset_source *s;
    rotmos_source_config config;
    rotmos_status status;
    rotmos_u64 end;
    if (out != NULL)
        *out = NULL;
    status = access(a, NULL, error);
    if (status != ROTMOS_OK)
        return status;
    if (out == NULL || a->config.info.kind != ROTMOS_ASSET_FILE)
        return failure(error, ROTMOS_INVALID,
                       "provide file asset and source output");
    status = rotmos_u64_add(offset, length, &end, error);
    if (status != ROTMOS_OK)
        return status;
    if (rotmos_asset_compare(end, a->config.info.size) > 0)
        return failure(error, ROTMOS_INVALID,
                       "asset source range exceeds the opened content");
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return failure(error, ROTMOS_NOMEM, "allocating asset source");
    s->asset = a;
    s->offset = offset;
    s->remaining = length;
    s->start = offset;
    s->length = length;
    rotmos_source_config_init(&config);
    config.read = source_read;
    config.reset = source_reset;
    config.close = source_close;
    config.userdata = s;
    config.has_length = 1;
    config.length = length;
    status = rotmos_source_new(&config, out, error);
    if (status != ROTMOS_OK) {
        free(s);
        return status;
    }
    a->owner = s;
    return ROTMOS_OK;
}
struct memory {
    rotmos_bytes bytes;
    rotmos_retire_fn close;
    void *userdata;
};
static rotmos_status memory_read(rotmos_u64 offset, void *bytes,
                                 size_t capacity, size_t *produced, void *data,
                                 rotmos_error *error)
{
    struct memory *s = data;
    size_t start;
    rotmos_status status = rotmos_u64_to_size(offset, &start, error);
    if (status != ROTMOS_OK)
        return status;
    if (start >= s->bytes.length) {
        *produced = 0;
        return ROTMOS_EOF;
    }
    *produced = s->bytes.length - start;
    if (*produced > capacity)
        *produced = capacity;
    memcpy(bytes, (const char *)s->bytes.data + start, *produced);
    return ROTMOS_OK;
}
static void memory_close(void *data)
{
    struct memory *s = data;
    if (s->close != NULL)
        s->close(s->userdata);
    free(s);
}
rotmos_status rotmos_asset_memory(const rotmos_asset_info *info,
                                  rotmos_bytes bytes, rotmos_retire_fn close,
                                  void *userdata, rotmos_asset **out,
                                  rotmos_error *error)
{
    struct memory *s;
    rotmos_asset_config config;
    rotmos_status status;
    rotmos_u64 size;
    if (out != NULL)
        *out = NULL;
    if (info == NULL || info->kind != ROTMOS_ASSET_FILE ||
        (bytes.data == NULL && bytes.length != 0))
        return failure(error, ROTMOS_INVALID,
                       "provide file metadata and borrowed byte span");
    rotmos_u64_from_size(bytes.length, &size, NULL);
    if (rotmos_asset_compare(info->size, size) != 0)
        return failure(error, ROTMOS_INVALID,
                       "asset metadata size differs from borrowed bytes");
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return failure(error, ROTMOS_NOMEM, "allocating borrowed memory asset");
    s->bytes = bytes;
    rotmos_asset_config_init(&config);
    config.info = *info;
    config.read = memory_read;
    config.close = memory_close;
    config.userdata = s;
    status = rotmos_asset_new(&config, out, error);
    if (status != ROTMOS_OK)
        free(s);
    else {
        s->close = close;
        s->userdata = userdata;
    }
    return status;
}
void rotmos_asset_provider_config_init(rotmos_asset_provider_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
rotmos_status
rotmos_asset_provider_new(const rotmos_asset_provider_config *config,
                          rotmos_asset_provider **out, rotmos_error *error)
{
    rotmos_asset_provider *p;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->open == NULL)
        return failure(error, ROTMOS_INVALID,
                       "provide asset open callback and provider output");
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return failure(error, ROTMOS_NOMEM, "allocating asset provider");
    p->config = *config;
    p->references = 1;
    *out = p;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status rotmos_asset_provider_open(rotmos_asset_provider *p,
                                         const char *path, rotmos_asset **out,
                                         rotmos_error *error)
{
    rotmos_status status;
    rotmos_asset *a = NULL;
    if (out != NULL)
        *out = NULL;
    if (p == NULL || out == NULL || !rotmos_asset_path_valid(path))
        return failure(
            error, ROTMOS_INVALID,
            "provide provider, normalized absolute asset path and output");
    if (p->closed || p->busy)
        return failure(error, ROTMOS_STATE, "asset provider is closed or busy");
    p->busy = 1;
    rotmos_error_clear(error);
    status = p->config.open(path, &a, p->config.userdata, error);
    p->busy = 0;
    if (status == ROTMOS_OK && a != NULL && a->provider == NULL &&
        a->owner == NULL && !a->busy) {
        a->provider = p;
        ++p->references;
        *out = a;
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (a != NULL && a->provider == NULL && a->owner == NULL)
        rotmos_asset_close(a, NULL);
    if (status == ROTMOS_EOF && a == NULL) {
        rotmos_error_clear(error);
        return status;
    }
    if (status == ROTMOS_OK || status == ROTMOS_AGAIN || status < ROTMOS_OK ||
        status > ROTMOS_BACKEND)
        status = ROTMOS_INVALID;
    return error != NULL && error->status == status
               ? status
               : failure(error, status,
                         "asset provider must synchronously return one new "
                         "asset or EOF");
}
rotmos_status rotmos_asset_provider_close(rotmos_asset_provider *p,
                                          rotmos_error *error)
{
    if (p == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (p->busy || p->closed)
        return failure(error, ROTMOS_STATE, "asset provider is closed or busy");
    p->closed = 1;
    provider_unref(p);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_asset_claim(rotmos_asset *asset, void *owner,
                                 rotmos_retire_fn release, void *data,
                                 rotmos_error *error)
{
    rotmos_status status = access(asset, NULL, error);
    if (status != ROTMOS_OK)
        return status;
    if (asset->retained_release != NULL)
        return failure(error, ROTMOS_STATE,
                       "storage result must be a new unclaimed asset");
    asset->owner = owner;
    asset->retained_release = release;
    asset->retained_data = data;
    return ROTMOS_OK;
}
void rotmos_asset_unclaim(rotmos_asset *asset, void *owner)
{
    if (asset != NULL && asset->owner == owner)
        asset->owner = NULL;
}
