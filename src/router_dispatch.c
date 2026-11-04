#include "request_internal.h"

#include <stdlib.h>
#include <string.h>

struct match_frame {
    const char *pattern;
    const char *path;
    size_t params;
    int phase;
};

/* Iterative backtracking preserves Vectis's skip-first optional parameters
 * without putting a request-controlled segment count on the C call stack. */
static rotmos_status params_match(rotmos_route *route, rotmos_request *request,
                                  int *matched, rotmos_error *error)
{
    struct match_frame *frames;
    struct match_frame *frame;
    const char *pattern_end;
    const char *path_end;
    const char *p;
    size_t depth;
    size_t capacity;
    size_t name_length;
    size_t value_length;
    int parameter;
    int optional;
    *matched = 0;
    capacity = 2;
    for (p = route->config.path; *p != '\0'; ++p)
        if (*p == '/')
            ++capacity;
    if (capacity > (size_t)-1 / sizeof(*frames) ||
        capacity > (size_t)-1 / sizeof(*request->params))
        return rotmos_fail(error, ROTMOS_OVERFLOW, "router_select",
                           "parameter match storage overflow");
    frames = calloc(capacity, sizeof(*frames));
    free(request->params);
    request->params = malloc(capacity * sizeof(*request->params));
    if (frames == NULL || request->params == NULL) {
        free(frames);
        return rotmos_fail(error, ROTMOS_NOMEM, "router_select",
                           "cannot allocate parameter matching storage");
    }
    depth = 1;
    frames[0].pattern = route->config.path + 1;
    frames[0].path = (const char *)request->path.data + 1;
    while (depth != 0) {
        frame = &frames[depth - 1];
        request->param_count = frame->params;
        if (*frame->pattern == '\0') {
            if (*frame->path == '\0') {
                *matched = 1;
                break;
            }
            --depth;
            continue;
        }
        pattern_end = strchr(frame->pattern, '/');
        if (pattern_end == NULL)
            pattern_end = frame->pattern + strlen(frame->pattern);
        path_end = strchr(frame->path, '/');
        if (path_end == NULL)
            path_end = frame->path + strlen(frame->path);
        parameter = *frame->pattern == ':';
        optional = parameter && pattern_end[-1] == '?';
        if (*frame->path == '\0') {
            if (optional && *pattern_end == '\0') {
                *matched = 1;
                break;
            }
            --depth;
            continue;
        }
        if (frame->phase == 0 && optional) {
            frame->phase = 1;
            frames[depth].pattern =
                *pattern_end != '\0' ? pattern_end + 1 : pattern_end;
            frames[depth].path = frame->path;
            frames[depth].params = frame->params;
            frames[depth++].phase = 0;
            continue;
        }
        if (frame->phase < 2) {
            frame->phase = 2;
            name_length = (size_t)(pattern_end - frame->pattern);
            value_length = (size_t)(path_end - frame->path);
            if (parameter) {
                request->params[request->param_count].name.data =
                    frame->pattern + 1;
                request->params[request->param_count].name.length =
                    name_length - 1 - (optional ? 1 : 0);
                request->params[request->param_count].value.data = frame->path;
                request->params[request->param_count++].value.length =
                    value_length;
            } else if (name_length != value_length ||
                       memcmp(frame->pattern, frame->path, value_length) != 0) {
                --depth;
                continue;
            }
            frames[depth].pattern =
                *pattern_end != '\0' ? pattern_end + 1 : pattern_end;
            frames[depth].path = *path_end != '\0' ? path_end + 1 : path_end;
            frames[depth].params = request->param_count;
            frames[depth++].phase = 0;
            continue;
        }
        --depth;
    }
    free(frames);
    if (!*matched)
        request->param_count = 0;
    return ROTMOS_OK;
}

static rotmos_status select_route(rotmos_router *router,
                                  rotmos_request *request, int raw,
                                  rotmos_error *error)
{
    size_t i;
    rotmos_route *route;
    rotmos_status status;
    int matched;
    request->route = NULL;
    request->param_count = 0;
    if (request->method <= ROTMOS_HTTP_ANY ||
        request->method > ROTMOS_HTTP_MOVE)
        return rotmos_fail(error, ROTMOS_INVALID, "router_select",
                           "unknown HTTP method");
    for (i = 0; i < router->count; ++i) {
        route = router->routes[i];
        if (raw && !route->raw_path)
            continue;
        if ((route->config.methods &
             ROTMOS_HTTP_METHOD_MASK(request->method)) == 0)
            continue;
        matched = 0;
        switch (route->config.path_kind) {
        case ROTMOS_PATH_LITERAL:
            matched = strcmp(route->config.path, request->path.data) == 0;
            break;
        case ROTMOS_PATH_REGEX:
            matched =
                regexec(&route->pattern, request->path.data, 0, NULL, 0) == 0;
            break;
        case ROTMOS_PATH_PARAMS:
            status = params_match(route, request, &matched, error);
            if (status != ROTMOS_OK)
                return status;
            break;
        }
        if (matched) {
            request->route = route;
            rotmos_error_clear(error);
            return ROTMOS_OK;
        }
    }
    rotmos_error_clear(error);
    return ROTMOS_EOF;
}

rotmos_status rotmos_router_select(rotmos_router *router,
                                   rotmos_request *request, rotmos_error *error)
{
    return select_route(router, request, request->raw_selected, error);
}

rotmos_status rotmos_router_select_raw(rotmos_router *router,
                                       rotmos_request *request,
                                       rotmos_error *error)
{
    return select_route(router, request, 1, error);
}

static rotmos_status hook(rotmos_route_hook_fn function,
                          const rotmos_route_hooks *hooks,
                          rotmos_request *request, int *finish,
                          rotmos_error *error)
{
    return function != NULL ? function(request, request->reply, finish,
                                       hooks->userdata, error)
                            : ROTMOS_OK;
}

rotmos_status rotmos_router_invoke(rotmos_request *request, rotmos_error *error)
{
    const rotmos_route_hooks *local;
    const rotmos_route_hooks *group;
    rotmos_route_hooks empty = {0};
    rotmos_status status;
    int finish;
    local = &request->route->config.hooks;
    group = request->route->group != NULL ? &request->route->group->config.hooks
                                          : &empty;
    finish = 0;
    status = hook(group->auth, group, request, &finish, error);
    if (status != ROTMOS_OK || finish)
        return status;
    status = hook(local->auth, local, request, &finish, error);
    if (status != ROTMOS_OK || finish)
        return status;
    status = hook(group->before, group, request, &finish, error);
    if (status != ROTMOS_OK || finish)
        return status;
    status = hook(local->before, local, request, &finish, error);
    if (status != ROTMOS_OK || finish)
        return status;
    status = request->route->config.handler(
        request, request->reply, request->route->config.userdata, error);
    if (status != ROTMOS_OK)
        return status;
    status = hook(local->after, local, request, &finish, error);
    if (status != ROTMOS_OK)
        return status;
    return hook(group->after, group, request, &finish, error);
}
