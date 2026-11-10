#ifndef KNURL_REPLY_INTERNAL_H
#define KNURL_REPLY_INTERNAL_H

#include "source_internal.h"

#include <knurl/reply.h>
#include <time.h>

knurl_status knurl_reply_failure(knurl_reply *, int, const char *);

struct knurl_reply {
    int selected;
    int status;
    int busy;
    void *owner;
    knurl_reply_kind kind;
    char *content_type;
    unsigned char *body;
    size_t body_length;
    int file;
    char *file_path;
    knurl_u64 file_length;
    time_t file_modified;
    knurl_source *source;
    knurl_source *retired_head;
    knurl_source *retired_tail;
    knurl_header *headers;
    size_t header_count;
};

void knurl_reply_retire(knurl_reply *reply);
knurl_status knurl_reply_open_file(knurl_reply *, knurl_error *);
typedef knurl_status (*knurl_reply_file_writer_fn)(int, void *, knurl_error *);
knurl_status knurl_reply_generate_file(knurl_reply *, int, const char *,
                                       const char *, knurl_reply_file_writer_fn,
                                       void *, knurl_error *);

#endif
