#ifndef KNURL_ASSET_INTERNAL_H
#define KNURL_ASSET_INTERNAL_H
#include <knurl/asset.h>
#include "internal.h"
int knurl_asset_path_valid(const char *);
int knurl_asset_compare(knurl_u64, knurl_u64);
knurl_u64 knurl_asset_subtract(knurl_u64, knurl_u64);
int knurl_asset_child_valid(const char *);
knurl_status knurl_asset_info_copy(const knurl_asset_info *, knurl_asset_info *,
                                   knurl_error *);
void knurl_asset_info_clear(knurl_asset_info *);
knurl_status knurl_asset_claim(knurl_asset *, void *, knurl_retire_fn, void *,
                               knurl_error *);
void knurl_asset_unclaim(knurl_asset *, void *);
knurl_status knurl_asset_source_claim(knurl_asset *, void *, knurl_error *);
knurl_status knurl_asset_read_owned(knurl_asset *, void *, knurl_u64, void *,
                                    size_t, size_t *, knurl_error *);
#endif
