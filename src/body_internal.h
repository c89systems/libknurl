#ifndef ROTMOS_BODY_INTERNAL_H
#define ROTMOS_BODY_INTERNAL_H

#include "internal.h"
#include <rotmos/body.h>
#include <sys/types.h>

struct rotmos_body {
    rotmos_body_policy policy;
    rotmos_u64 length;
    rotmos_u64 position;
    unsigned char *memory;
    size_t capacity;
    size_t memory_limit;
    char *directory;
    char *path;
    int fd;
    int complete;
    rotmos_status failure;
};

int rotmos_file_offset(rotmos_u64 value, off_t *out);
rotmos_status rotmos_private_file(const char *directory, const char *prefix,
                                  int *fd, char **path, rotmos_error *error);
rotmos_status rotmos_file_write(int fd, const void *data, size_t length,
                                rotmos_error *error);
rotmos_status rotmos_body_create(const rotmos_body_policy *policy,
                                 const char *directory, rotmos_body **out,
                                 rotmos_error *error);
rotmos_status rotmos_body_append(rotmos_body *body, const void *data,
                                 size_t length, rotmos_error *error);
void rotmos_body_release(rotmos_body *body);

#endif
