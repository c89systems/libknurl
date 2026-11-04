#ifndef ROTMOS_WEBDAV_INTERNAL_H
#define ROTMOS_WEBDAV_INTERNAL_H
#include <rotmos/webdav.h>
#include <rotmos/request.h>
#include <rotmos/body.h>
#include "asset_internal.h"
#include "request_internal.h"
#include "response_internal.h"
struct dav_route {
    rotmos_webdav_config config;
    rotmos_storage_provider *provider;
    char *prefix;
    unsigned int capabilities;
    int own_provider;
};
struct dav_request {
    struct dav_route *route;
    rotmos_request *request;
    rotmos_storage_operation *pending;
    rotmos_storage_resource *writer, *list;
    rotmos_asset *asset;
    rotmos_storage_action action;
    rotmos_http_method method;
    char *path, *destination, *item_path;
    rotmos_asset_info item_info;
    char principal[ROTMOS_WEBDAV_PRINCIPAL_MAX + 1], number[21];
    size_t references, accepted;
    int depth, overwrite, body_end, denied, finalized, stopped;
    int prop_phase, prop_part, href_escape;
    size_t prop_offset, href_offset;
};
rotmos_status rotmos_dav_headers(rotmos_request *, rotmos_request_plan *,
                                 void *, rotmos_error *);
rotmos_status rotmos_dav_start(struct dav_request *, rotmos_storage_action,
                               rotmos_bytes, rotmos_error *);
rotmos_status rotmos_dav_finish(struct dav_request *, rotmos_error *);
void rotmos_dav_error(struct dav_request *, int, rotmos_status);
void rotmos_dav_ref(struct dav_request *);
void rotmos_dav_unref(struct dav_request *);
rotmos_status rotmos_dav_authorize(struct dav_request *, const char *, int,
                                   int *, rotmos_error *);
rotmos_status rotmos_dav_propfind(struct dav_request *, rotmos_error *);
void rotmos_dav_prop_entry(struct dav_request *, const rotmos_storage_result *);
#endif
