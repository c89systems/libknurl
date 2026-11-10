#ifndef KNURL_INTERNAL_H
#define KNURL_INTERNAL_H

#include <knurl/common.h>

knurl_status knurl_fail(knurl_error *error, knurl_status status,
                        const char *operation, const char *message);
int knurl_u64_valid(knurl_u64 value);
char *knurl_copy_text(const char *text);
int knurl_header_name_valid(const char *text);
int knurl_header_value_valid(const char *text);
int knurl_content_type_valid(const char *text);
int knurl_text_equal_case(const char *left, const char *right);

#endif
