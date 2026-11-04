#ifndef ROTMOS_STATIC_INTERNAL_H
#define ROTMOS_STATIC_INTERNAL_H
#include "asset_internal.h"
#include <rotmos/static.h>
rotmos_status rotmos_static_reply_asset(const rotmos_static_config *, int,
                                        rotmos_request *, rotmos_reply *,
                                        rotmos_asset *, const char *,
                                        rotmos_error *);
struct rotmos_static_range {
    rotmos_u64 start, length;
    int requested, satisfiable;
};
int rotmos_http_tag_matches(const char *, const char *, int, int);
int rotmos_static_if_range(const char *, const char *);
void rotmos_static_range_parse(const char *, rotmos_u64,
                               struct rotmos_static_range *);
#endif
