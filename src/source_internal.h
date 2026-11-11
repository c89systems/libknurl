#ifndef KNURL_SOURCE_INTERNAL_H
#define KNURL_SOURCE_INTERNAL_H

#include "internal.h"
#include <knurl/io.h>
#include "executor.h"

struct knurl_source_impl {
    knurl_source receiver;
    knurl_source_config config;
    knurl_u64 position;
    void *owner;
    int borrowed;
    unsigned int cancelled;
    int resume_requested;
    knurl_job *native_job;
    void *native;
    knurl_loop *loop;
    void (*resume)(void *);
    void *resume_data;
    int busy;
    int eof;
    knurl_status failure;
    knurl_source *retired_next;
};

knurl_status knurl_source_pull(knurl_source *source, void *owner, void *buffer,
                               size_t capacity, size_t *produced,
                               knurl_error *error);
knurl_status knurl_source_validate(knurl_source *, size_t, size_t, knurl_status,
                                   knurl_error *);
knurl_status knurl_source_native_pull(knurl_source *, knurl_response *, void *,
                                      size_t, size_t *, knurl_error *);
void knurl_source_native_revoke(knurl_source *);
void knurl_source_revoke(knurl_source *);
void knurl_source_native_retire(knurl_source *, knurl_status);
void knurl_source_native_resume(knurl_source *);
void knurl_source_release_now(knurl_source *, knurl_status);
void knurl_source_release(knurl_source *source, knurl_status outcome);

#endif
