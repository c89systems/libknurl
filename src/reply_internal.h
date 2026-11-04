#ifndef ROTMOS_REPLY_INTERNAL_H
#define ROTMOS_REPLY_INTERNAL_H

#include "source_internal.h"

#include <rotmos/reply.h>

rotmos_status rotmos_reply_failure(rotmos_reply *, int, const char *);

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
    char *file_path;
    rotmos_u64 file_length;
    rotmos_source *source;
    rotmos_source *retired_head;
    rotmos_source *retired_tail;
    rotmos_header *headers;
    size_t header_count;
};

void rotmos_reply_retire(rotmos_reply *reply);
rotmos_status rotmos_reply_open_file(rotmos_reply *, rotmos_error *);
typedef rotmos_status (*rotmos_reply_file_writer_fn)(int, void *,
                                                     rotmos_error *);
rotmos_status rotmos_reply_generate_file(rotmos_reply *, int, const char *,
                                         const char *,
                                         rotmos_reply_file_writer_fn, void *,
                                         rotmos_error *);

#endif
