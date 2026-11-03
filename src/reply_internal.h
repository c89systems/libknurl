#ifndef ROTMOS_REPLY_INTERNAL_H
#define ROTMOS_REPLY_INTERNAL_H

#include "source_internal.h"
#include <rotmos/reply.h>

struct rotmos_reply {
    int selected;
    int status;
    int busy;
    void *owner;
    rotmos_reply_kind kind;
    char *content_type;
    unsigned char *body;
    size_t body_length;
    int file;
    rotmos_u64 file_length;
    rotmos_source *source;
    rotmos_source *retired_head;
    rotmos_source *retired_tail;
    rotmos_header *headers;
    size_t header_count;
};

void rotmos_reply_retire(rotmos_reply *reply);

#endif
