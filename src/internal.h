#ifndef ROTMOS_INTERNAL_H
#define ROTMOS_INTERNAL_H

#include <rotmos/common.h>

rotmos_status rotmos_fail(rotmos_error *error, rotmos_status status,
                          const char *operation, const char *message);
int rotmos_u64_valid(rotmos_u64 value);
char *rotmos_copy_text(const char *text);
int rotmos_header_name_valid(const char *text);
int rotmos_header_value_valid(const char *text);
int rotmos_text_equal_case(const char *left, const char *right);

#endif
