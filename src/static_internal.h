#ifndef KNURL_STATIC_INTERNAL_H
#define KNURL_STATIC_INTERNAL_H
#include "asset_internal.h"
#include <knurl/static.h>
#include "mime_internal.h"
knurl_status knurl_static_reply_asset(const knurl_static_config *, int,
                                      knurl_request *, knurl_reply *,
                                      knurl_asset_provider *, knurl_asset *,
                                      const char *, knurl_error *);
struct knurl_static_range_part {
    knurl_u64 start, length;
};
#define KNURL_STATIC_MAX_RANGES 16
knurl_status knurl_static_gzip_take(knurl_source *, knurl_source **,
                                    knurl_error *);
struct knurl_static_encoding {
    knurl_asset *asset;
    knurl_asset_info info;
    const char *coding;
    char etag[KNURL_ASSET_ETAG_MAX + 16];
    int gzip;
};
knurl_status knurl_static_encoding_select(
    const knurl_static_config *, knurl_asset_provider *, knurl_request *,
    knurl_reply *, knurl_asset *, const char *, const char *,
    struct knurl_static_encoding *, knurl_error *);
struct knurl_static_ranges {
    struct knurl_static_range_part parts[KNURL_STATIC_MAX_RANGES];
    size_t count;
    int requested, satisfiable;
};
int knurl_http_tag_matches(const char *, const char *, int, int);
int knurl_static_if_range(const char *, const knurl_asset_info *);
void knurl_static_range_parse(const char *, knurl_u64,
                              struct knurl_static_ranges *);
knurl_status knurl_static_multipart_take(knurl_asset *,
                                         const struct knurl_static_ranges *,
                                         const char *, const char *,
                                         knurl_source **, knurl_error *);
#endif
