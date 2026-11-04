#ifndef ROTMOS_FILE_INTERNAL_H
#define ROTMOS_FILE_INTERNAL_H
#include "internal.h"
#include <sys/types.h>
int rotmos_file_offset(rotmos_u64 value, off_t *out);
rotmos_status rotmos_private_file(const char *directory, const char *prefix,
                                  int *fd, char **path, rotmos_error *error);
rotmos_status rotmos_file_write(int fd, const void *data, size_t length,
                                rotmos_error *error);
#endif
