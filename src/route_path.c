#include "router_internal.h"

#include <string.h>

static int name_start(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           byte == '_';
}

static int name_byte(unsigned char byte)
{
    return name_start(byte) || (byte >= '0' && byte <= '9');
}

rotmos_status rotmos_route_path_validate(const char *path,
                                         rotmos_route_path_kind kind,
                                         rotmos_error *error)
{
    const char *p;
    int segment_start;
    if (kind == ROTMOS_PATH_REGEX) {
        if (path == NULL || path[0] != '^' || path[1] != '/')
            return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                               "regex route path must start with '^/'");
        return ROTMOS_OK;
    }
    if ((kind != ROTMOS_PATH_LITERAL && kind != ROTMOS_PATH_PARAMS) ||
        path == NULL || path[0] != '/')
        return rotmos_fail(
            error, ROTMOS_INVALID, "router_route",
            "route requires a known path kind and path starting with '/'");
    if (kind == ROTMOS_PATH_LITERAL && strchr(path, ':') != NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "parameters require PARAMS path kind");
    p = path + 1;
    segment_start = 1;
    while (*p != '\0') {
        if (*p == '/') {
            if (segment_start)
                return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                                   "route path contains an empty segment");
            segment_start = 1;
            ++p;
            continue;
        }
        if (segment_start && p[0] == '.' &&
            (p[1] == '/' || p[1] == '\0' ||
             (p[1] == '.' && (p[2] == '/' || p[2] == '\0'))))
            return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                               "route path contains a dot segment");
        if (*p == ':') {
            if (!segment_start)
                return rotmos_fail(
                    error, ROTMOS_INVALID, "router_route",
                    "parameter must occupy a complete path segment");
            ++p;
            if (!name_start((unsigned char)*p))
                return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                                   "invalid path parameter name");
            while (name_byte((unsigned char)*p))
                ++p;
            if (*p == '?')
                ++p;
            if (*p != '/' && *p != '\0')
                return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                                   "invalid path parameter name");
            segment_start = 0;
            continue;
        }
        if (*p == '?' || *p == '%' || *p == '\\')
            return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                               "route path has an invalid optional marker, "
                               "percent escape or backslash");
        segment_start = 0;
        ++p;
    }
    if (segment_start && path[1] != '\0')
        return rotmos_fail(error, ROTMOS_INVALID, "router_route",
                           "route path ends with an empty segment");
    return ROTMOS_OK;
}
