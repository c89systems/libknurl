#ifndef ROTMOS_SOURCE_INTERNAL_H
#define ROTMOS_SOURCE_INTERNAL_H

#include "internal.h"
#include <rotmos/io.h>

struct rotmos_source {
    rotmos_source_config config;
    rotmos_u64 position;
    void *owner;
    int busy;
    int eof;
    rotmos_status failure;
};

rotmos_status rotmos_source_pull(rotmos_source *source, void *owner,
                                 void *buffer, size_t capacity,
                                 size_t *produced, rotmos_error *error);
void rotmos_source_release(rotmos_source *source, rotmos_status outcome);

#endif
