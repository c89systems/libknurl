#ifndef KNURL_ROUTER_INTERNAL_H
#define KNURL_ROUTER_INTERNAL_H

#include "internal.h"
#include <knurl/router.h>
#include <regex.h>

struct knurl_route {
    knurl_route_config config;
    regex_t pattern;
    int has_pattern;
    int raw_path;
    int directory_mount;
    knurl_headers_fn preflight;
    knurl_route_group *group;
    void (*release_userdata)(void *);
};

struct knurl_route_group {
    knurl_router *router;
    knurl_route_group_config config;
    knurl_route_group *next;
};

struct knurl_router {
    struct knurl_openapi_entry *openapi;
    knurl_route **routes;
    size_t count;
    knurl_route_group *groups;
    knurl_server *server;
    int frozen;
};

knurl_status knurl_route_path_validate(const char *path,
                                       knurl_route_path_kind kind,
                                       knurl_error *error);
void knurl_router_release(knurl_router *router);
/* Built-in streaming facades explicitly run the staged hook phases. */
knurl_status knurl_router_facade_route(knurl_router *, knurl_route_group *,
                                       const knurl_route_config *,
                                       knurl_route **, knurl_error *);

#endif
