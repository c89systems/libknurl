#ifndef ROTMOS_ASSET_INTERNAL_H
#define ROTMOS_ASSET_INTERNAL_H
#include <rotmos/asset.h>
#include "internal.h"
int rotmos_asset_path_valid(const char *);
int rotmos_asset_compare(rotmos_u64, rotmos_u64);
rotmos_u64 rotmos_asset_subtract(rotmos_u64, rotmos_u64);
#endif
