#ifndef ROTMOS_PROXY_COMMON_H
#define ROTMOS_PROXY_COMMON_H
#include "internal.h"
#include <stdint.h>
int rotmos_proxy_format_length(uint64_t, char *);
void rotmos_proxy_error(rotmos_error *, rotmos_status, const char *);
rotmos_status rotmos_proxy_validate_path(const char *, rotmos_error *);
#endif
