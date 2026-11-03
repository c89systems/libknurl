#ifndef ROTMOS_INTERNAL_H
#define ROTMOS_INTERNAL_H

#include <rotmos/common.h>

rotmos_status rotmos_fail(rotmos_error *error, rotmos_status status,
                          const char *operation, const char *message);
int rotmos_u64_valid(rotmos_u64 value);

#endif
