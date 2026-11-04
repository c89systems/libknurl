#ifndef ROTMOS_ASSET_INTERNAL_H
#define ROTMOS_ASSET_INTERNAL_H
#include <rotmos/asset.h>
#include "internal.h"
int rotmos_asset_path_valid(const char *);
int rotmos_asset_compare(rotmos_u64, rotmos_u64);
rotmos_u64 rotmos_asset_subtract(rotmos_u64, rotmos_u64);
int rotmos_asset_child_valid(const char *);
rotmos_status rotmos_asset_info_copy(const rotmos_asset_info *,
                                     rotmos_asset_info *, rotmos_error *);
void rotmos_asset_info_clear(rotmos_asset_info *);
rotmos_status rotmos_asset_claim(rotmos_asset *, void *, rotmos_retire_fn,
                                 void *, rotmos_error *);
void rotmos_asset_unclaim(rotmos_asset *, void *);
#endif
