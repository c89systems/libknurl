#include "webdav_internal.h"
#include <stdlib.h>
#include <string.h>
static const char intro[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus "
    "xmlns:D=\"DAV:\">";
static const char outro[] = "</D:multistatus>";
static int emit(struct dav_request *s, const char *text, unsigned char *bytes,
                size_t capacity, size_t *used)
{
    size_t n = strlen(text), take = n - s->prop_offset;
    if (take > capacity - *used)
        take = capacity - *used;
    memcpy(bytes + *used, text + s->prop_offset, take);
    *used += take;
    s->prop_offset += take;
    if (s->prop_offset != n)
        return 0;
    s->prop_offset = 0;
    return 1;
}
static int href(struct dav_request *s, unsigned char *bytes, size_t capacity,
                size_t *used)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *path = s->item_path != NULL ? s->item_path : s->path;
    const char *prefix = s->route->prefix;
    size_t prefix_length = strcmp(prefix, "/") == 0 ? 0 : strlen(prefix);
    size_t path_length =
        prefix_length != 0 && strcmp(path, "/") == 0 ? 0 : strlen(path);
    unsigned char c;
    while (*used < capacity && s->href_offset < prefix_length + path_length) {
        c = (unsigned char)(s->href_offset < prefix_length
                                ? prefix[s->href_offset]
                                : path[s->href_offset - prefix_length]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || strchr("-._~/", c) != NULL) {
            bytes[(*used)++] = c;
            ++s->href_offset;
        } else {
            bytes[(*used)++] =
                s->href_escape == 0
                    ? '%'
                    : (s->href_escape == 1 ? (unsigned char)hex[c >> 4]
                                           : (unsigned char)hex[c & 15]);
            if (++s->href_escape == 3) {
                s->href_escape = 0;
                ++s->href_offset;
            }
        }
    }
    return s->href_offset == prefix_length + path_length;
}
static const char *part(struct dav_request *s)
{
    switch (s->prop_part) {
    case 0:
        return "<D:response><D:href>";
    case 2:
        return "</D:href><D:propstat><D:prop><D:resourcetype>";
    case 3:
        return s->item_info.kind == KNURL_ASSET_DIRECTORY ? "<D:collection/>"
                                                          : "";
    case 4:
        return "</D:resourcetype>";
    case 5:
        return s->item_info.kind == KNURL_ASSET_FILE ? "<D:getcontentlength>"
                                                     : "";
    case 6:
        return s->item_info.kind == KNURL_ASSET_FILE ? s->number : "";
    case 7:
        return s->item_info.kind == KNURL_ASSET_FILE ? "</D:getcontentlength>"
                                                     : "";
    case 8:
        return "</D:prop><D:status>HTTP/1.1 200 "
               "OK</D:status></D:propstat></D:response>";
    default:
        return "";
    }
}
static knurl_status read_prop(void *buffer, size_t capacity, size_t *produced,
                              void *data, knurl_error *error)
{
    struct dav_request *s = data;
    unsigned char *bytes = buffer;
    knurl_bytes empty = {0};
    int complete;
    *produced = 0;
    if (s->stopped)
        return KNURL_CANCELLED;
    while (*produced < capacity) {
        if (s->prop_phase == 0) {
            if (!emit(s, intro, bytes, capacity, produced))
                break;
            s->prop_phase = 1;
        } else if (s->prop_phase == 1) {
            complete = s->prop_part == 1
                           ? href(s, bytes, capacity, produced)
                           : emit(s, part(s), bytes, capacity, produced);
            if (!complete)
                break;
            if (++s->prop_part == 9) {
                s->prop_phase = 2;
                break;
            }
        } else if (s->prop_phase == 2) {
            if (*produced != 0)
                break;
            if (s->list == NULL) {
                s->prop_phase = 3;
                continue;
            }
            if (s->pending == NULL &&
                knurl_dav_start(s, KNURL_STORAGE_LIST_NEXT, empty, error) !=
                    KNURL_OK)
                return KNURL_IO;
            return KNURL_AGAIN;
        } else if (s->prop_phase == 3) {
            if (!emit(s, outro, bytes, capacity, produced))
                break;
            s->prop_phase = 4;
        } else
            return *produced != 0 ? KNURL_OK : KNURL_EOF;
    }
    return KNURL_OK;
}
static void close_prop(void *data, knurl_status cause)
{
    struct dav_request *s = data;
    (void)cause;
    s->stopped = 1;
    if (s->pending != NULL)
        (void)knurl_storage_operation_cancel(s->pending, NULL);
    if (s->list != NULL) {
        knurl_storage_resource_close(s->list, NULL);
        s->list = NULL;
    }
    knurl_dav_unref(s);
}
knurl_status knurl_dav_propfind(struct dav_request *s, knurl_error *error)
{
    knurl_source_config config;
    knurl_source *source;
    knurl_status status;
    size_t n;
    knurl_source_config_init(&config);
    config.read = read_prop;
    config.close = close_prop;
    config.userdata = s;
    (void)knurl_u64_format_decimal(s->item_info.size, s->number,
                                   sizeof(s->number), &n, NULL);
    status = knurl_source_new(&config, &source, error);
    if (status != KNURL_OK)
        return status;
    knurl_dav_ref(s);
    status = knurl_reply_source_take(s->request->impl->reply, 207,
                                     "application/xml; charset=utf-8", source,
                                     error);
    if (status != KNURL_OK) {
        knurl_source_destroy(source, NULL);
        return status;
    }
    return knurl_dav_finish(s, error);
}
void knurl_dav_prop_entry(struct dav_request *s,
                          const knurl_storage_result *result)
{
    knurl_error error;
    knurl_bytes empty = {0};
    size_t n = strlen(s->path), child = strlen(result->entry.name), written;
    char *path;
    int allowed;
    if (n + child + 1 > KNURL_ASSET_PATH_MAX) {
        knurl_dav_error(s, 413, KNURL_LIMIT);
        return;
    }
    path = malloc(n + child + 2);
    if (path == NULL) {
        knurl_dav_error(s, 500, KNURL_NOMEM);
        return;
    }
    strcpy(path, s->path);
    if (n > 1)
        path[n++] = '/';
    strcpy(path + n, result->entry.name);
    if (knurl_dav_authorize(s, path, 1, &allowed, &error) != KNURL_OK) {
        free(path);
        knurl_dav_error(s, 500, error.status);
        return;
    }
    if (!allowed) {
        free(path);
        if (knurl_dav_start(s, KNURL_STORAGE_LIST_NEXT, empty, &error) !=
            KNURL_OK)
            knurl_dav_error(s, 500, error.status);
        return;
    }
    free(s->item_path);
    s->item_path = path;
    knurl_asset_info_clear(&s->item_info);
    if (knurl_asset_info_copy(&result->entry.info, &s->item_info, &error) !=
        KNURL_OK) {
        knurl_dav_error(s, 500, error.status);
        return;
    }
    (void)knurl_u64_format_decimal(s->item_info.size, s->number,
                                   sizeof(s->number), &written, NULL);
    s->prop_phase = 1;
    s->prop_part = 0;
    s->prop_offset = s->href_offset = 0;
    s->href_escape = 0;
    (void)knurl_response_producer_resume(knurl_request_response(s->request),
                                         NULL);
}
