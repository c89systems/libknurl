#ifndef ROTMOS_ROUTER_INTERNAL_H
#define ROTMOS_ROUTER_INTERNAL_H

#include "internal.h"
#include <rotmos/router.h>
#include <regex.h>

struct rotmos_route {
    rotmos_route_config config;
    regex_t pattern;
    int has_pattern;
    int raw_path;
    int directory_mount;
    rotmos_headers_fn preflight;
    rotmos_route_group *group;
    void (*release_userdata)(void *);
};

struct rotmos_route_group {
    rotmos_router *router;
    rotmos_route_group_config config;
    rotmos_route_group *next;
};

struct rotmos_router {
    rotmos_route **routes;
    size_t count;
    rotmos_route_group *groups;
    rotmos_server *server;
    int frozen;
};

rotmos_status rotmos_route_path_validate(const char *path,
                                         rotmos_route_path_kind kind,
                                         rotmos_error *error);
void rotmos_router_release(rotmos_router *router);
/* Built-in streaming facades explicitly run the staged hook phases. */
rotmos_status rotmos_router_facade_route(rotmos_router *, rotmos_route_group *,
                                         const rotmos_route_config *,
                                         rotmos_route **, rotmos_error *);

#endif
