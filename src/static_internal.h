#ifndef KNURL_STATIC_INTERNAL_H
#define KNURL_STATIC_INTERNAL_H
#include "asset_internal.h"
#include <knurl/static.h>
#include "mime_internal.h"
knurl_status knurl_static_reply_asset(const knurl_static_config *, int,
                                      knurl_request *, knurl_reply *,
                                      knurl_asset *, const char *,
                                      knurl_error *);
struct knurl_static_range {
    knurl_u64 start, length;
    int requested, satisfiable;
};
int knurl_http_tag_matches(const char *, const char *, int, int);
int knurl_static_if_range(const char *, const char *);
void knurl_static_range_parse(const char *, knurl_u64,
                              struct knurl_static_range *);
#endif
