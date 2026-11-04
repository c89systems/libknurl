#ifndef ROTMOS_STATIC_INTERNAL_H
#define ROTMOS_STATIC_INTERNAL_H
#include "asset_internal.h"
struct rotmos_static_range {
    rotmos_u64 start, length;
    int requested, satisfiable;
};
int rotmos_http_tag_matches(const char *, const char *, int, int);
int rotmos_static_if_range(const char *, const char *);
void rotmos_static_range_parse(const char *, rotmos_u64,
                               struct rotmos_static_range *);
#endif
