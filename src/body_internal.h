#ifndef KNURL_BODY_INTERNAL_H
#define KNURL_BODY_INTERNAL_H

#include "internal.h"
#include "file_internal.h"
#include <knurl/body.h>
#include <sys/types.h>

struct knurl_body_impl {
    knurl_body receiver;
    knurl_body_policy policy;
    knurl_u64 length;
    knurl_u64 position;
    unsigned char *memory;
    size_t capacity;
    size_t memory_limit;
    char *directory;
    char *path;
    int fd;
    int complete;
    knurl_status failure;
};

knurl_u64 knurl_body_policy_limit(const knurl_body_policy *, knurl_u64);
knurl_status knurl_body_create(const knurl_body_policy *policy,
                               const char *directory, knurl_body **out,
                               knurl_error *error);
knurl_status knurl_body_append(knurl_body *body, const void *data,
                               size_t length, knurl_error *error);
void knurl_body_release(knurl_body *body);

#endif
