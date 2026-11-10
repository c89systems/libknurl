#ifndef KNURL_WEBDAV_INTERNAL_H
#define KNURL_WEBDAV_INTERNAL_H
#include <knurl/webdav.h>
#include <knurl/request.h>
#include <knurl/body.h>
#include "asset_internal.h"
#include "request_internal.h"
#include "response_internal.h"
struct dav_route {
    knurl_webdav_config config;
    knurl_storage_provider *provider;
    char *prefix;
    unsigned int capabilities;
    int own_provider;
};
struct dav_request {
    struct dav_route *route;
    knurl_request *request;
    knurl_storage_operation *pending;
    knurl_storage_resource *writer, *list;
    knurl_asset *asset;
    knurl_storage_action action;
    knurl_http_method method;
    char *path, *destination, *item_path;
    knurl_asset_info item_info;
    char principal[KNURL_WEBDAV_PRINCIPAL_MAX + 1], number[21];
    size_t references, accepted;
    int depth, overwrite, body_end, denied, finalized, stopped;
    int prop_phase, prop_part, href_escape;
    size_t prop_offset, href_offset;
};
knurl_status knurl_dav_headers(knurl_request *, knurl_request_plan *, void *,
                               knurl_error *);
knurl_status knurl_dav_start(struct dav_request *, knurl_storage_action,
                             knurl_bytes, knurl_error *);
knurl_status knurl_dav_finish(struct dav_request *, knurl_error *);
void knurl_dav_error(struct dav_request *, int, knurl_status);
void knurl_dav_ref(struct dav_request *);
void knurl_dav_unref(struct dav_request *);
knurl_status knurl_dav_authorize(struct dav_request *, const char *, int, int *,
                                 knurl_error *);
knurl_status knurl_dav_propfind(struct dav_request *, knurl_error *);
void knurl_dav_prop_entry(struct dav_request *, const knurl_storage_result *);
#endif
