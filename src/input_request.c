#include "request_internal.h"
#include <knurl/input.h>

#include <string.h>

static knurl_status value(const knurl_request *request, const char *name,
                          int path, knurl_bytes *out, knurl_error *error)
{
    knurl_header pair;
    size_t i, found = 0, length;
    if (request == NULL || name == NULL || name[0] == 0)
        return knurl_fail(error, KNURL_INVALID, "input_request",
                          "provide a live request and parameter name");
    if (path)
        return knurl_request_path_param(request, name, out, error);
    length = strlen(name);
    for (i = 0; i < knurl_request_query_count(request); ++i) {
        (void)knurl_request_query_at(request, i, &pair, NULL);
        if (pair.name.length == length &&
            memcmp(pair.name.data, name, length) == 0) {
            *out = pair.value;
            ++found;
        }
    }
    if (found != 1)
        return knurl_fail(error, found == 0 ? KNURL_EOF : KNURL_INVALID,
                          "input_request",
                          found == 0 ? "parameter is absent"
                                     : "parameter must occur exactly once");
    return KNURL_OK;
}
static knurl_status integer(const knurl_request *r, const char *name, int path,
                            long minimum, long maximum, long *out,
                            knurl_error *e)
{
    knurl_bytes bytes;
    knurl_status status = value(r, name, path, &bytes, e);
    return status == KNURL_OK
               ? knurl_input_long(bytes, minimum, maximum, out, e)
               : status;
}
static knurl_status count(const knurl_request *r, const char *name, int path,
                          knurl_u64 minimum, knurl_u64 maximum, knurl_u64 *out,
                          knurl_error *e)
{
    knurl_bytes bytes;
    knurl_status status = value(r, name, path, &bytes, e);
    return status == KNURL_OK ? knurl_input_u64(bytes, minimum, maximum, out, e)
                              : status;
}
static knurl_status boolean(const knurl_request *r, const char *name, int path,
                            int *out, knurl_error *e)
{
    knurl_bytes bytes;
    knurl_status status = value(r, name, path, &bytes, e);
    return status == KNURL_OK ? knurl_input_boolean(bytes, out, e) : status;
}
knurl_status knurl_request_query_long(const knurl_request *r, const char *name,
                                      long minimum, long maximum, long *out,
                                      knurl_error *e)
{
    return integer(r, name, 0, minimum, maximum, out, e);
}
knurl_status knurl_request_param_long(const knurl_request *r, const char *name,
                                      long minimum, long maximum, long *out,
                                      knurl_error *e)
{
    return integer(r, name, 1, minimum, maximum, out, e);
}
knurl_status knurl_request_query_u64(const knurl_request *r, const char *name,
                                     knurl_u64 minimum, knurl_u64 maximum,
                                     knurl_u64 *out, knurl_error *e)
{
    return count(r, name, 0, minimum, maximum, out, e);
}
knurl_status knurl_request_param_u64(const knurl_request *r, const char *name,
                                     knurl_u64 minimum, knurl_u64 maximum,
                                     knurl_u64 *out, knurl_error *e)
{
    return count(r, name, 1, minimum, maximum, out, e);
}
knurl_status knurl_request_query_boolean(const knurl_request *r,
                                         const char *name, int *out,
                                         knurl_error *e)
{
    return boolean(r, name, 0, out, e);
}
knurl_status knurl_request_param_boolean(const knurl_request *r,
                                         const char *name, int *out,
                                         knurl_error *e)
{
    return boolean(r, name, 1, out, e);
}
