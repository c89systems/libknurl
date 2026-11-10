#ifndef KNURL_PROXY_COMMON_H
#define KNURL_PROXY_COMMON_H
#include "internal.h"
#include <stdint.h>
int knurl_proxy_format_length(uint64_t, char *);
void knurl_proxy_error(knurl_error *, knurl_status, const char *);
knurl_status knurl_proxy_validate_path(const char *, knurl_error *);
#endif
