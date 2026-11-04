#ifndef ROTMOS_SOURCE_INTERNAL_H
#define ROTMOS_SOURCE_INTERNAL_H

#include "internal.h"
#include <rotmos/io.h>
#include "executor.h"

struct rotmos_source {
    rotmos_source_config config;
    rotmos_u64 position;
    void *owner;
    int borrowed;
    unsigned int cancelled;
    int resume_requested;
    rotmos_job *native_job;
    void *native;
    rotmos_loop *loop;
    void (*resume)(void *);
    void *resume_data;
    int busy;
    int eof;
    rotmos_status failure;
    rotmos_source *retired_next;
};

rotmos_status rotmos_source_pull(rotmos_source *source, void *owner,
                                 void *buffer, size_t capacity,
                                 size_t *produced, rotmos_error *error);
rotmos_status rotmos_source_validate(rotmos_source *, size_t, size_t,
                                     rotmos_status, rotmos_error *);
rotmos_status rotmos_source_native_pull(rotmos_source *, rotmos_response *,
                                        void *, size_t, size_t *,
                                        rotmos_error *);
void rotmos_source_native_revoke(rotmos_source *);
void rotmos_source_revoke(rotmos_source *);
void rotmos_source_native_retire(rotmos_source *, rotmos_status);
void rotmos_source_native_resume(rotmos_source *);
void rotmos_source_release_now(rotmos_source *, rotmos_status);
void rotmos_source_release(rotmos_source *source, rotmos_status outcome);

#endif
