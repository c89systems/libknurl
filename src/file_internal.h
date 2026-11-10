#ifndef KNURL_FILE_INTERNAL_H
#define KNURL_FILE_INTERNAL_H
#include "internal.h"
#include <sys/types.h>
int knurl_file_offset(knurl_u64 value, off_t *out);
knurl_status knurl_private_file(const char *directory, const char *prefix,
                                int *fd, char **path, knurl_error *error);
knurl_status knurl_file_write(int fd, const void *data, size_t length,
                              knurl_error *error);
#endif
