#include "upload_internal.h"
#include <stdlib.h>
#include <string.h>
/* One string slab and one ordered pair slab, both retained through native ACK.
 */
rotmos_status rotmos_upload_metadata_copy(rotmos_request *r,
                                          rotmos_upload_metadata *m,
                                          void **text, rotmos_header **pairs,
                                          rotmos_error *error)
{
    rotmos_bytes input[11], *output[11];
    const rotmos_header *groups[3];
    size_t sizes[3], i, j, total = 0, count = 0;
    char *cursor;
    rotmos_header *p;
    memset(m, 0, sizeof(*m));
    *text = NULL;
    *pairs = NULL;
    input[0] = r->method_token;
    output[0] = &m->method_token;
    input[1] = r->version;
    output[1] = &m->version;
    input[2] = r->target;
    output[2] = &m->target;
    input[3] = r->raw_path;
    output[3] = &m->raw_path;
    input[4] = r->path;
    output[4] = &m->path;
    input[5] = r->query;
    output[5] = &m->query;
    input[6] = r->authority;
    output[6] = &m->authority;
    input[7] = r->peer.address;
    output[7] = &m->peer.address;
    input[8] = r->local.address;
    output[8] = &m->local.address;
    input[9] = r->effective_address;
    output[9] = &m->effective_address;
    input[10] = r->correlation_id;
    output[10] = &m->correlation_id;
    groups[0] = r->headers;
    sizes[0] = r->header_count;
    groups[1] = r->queries;
    sizes[1] = r->query_count;
    groups[2] = r->params;
    sizes[2] = r->param_count;
    for (i = 0; i < 11; ++i) {
        if (input[i].length == (size_t)-1 ||
            total > (size_t)-1 - input[i].length - 1)
            goto overflow;
        total += input[i].length + 1;
    }
    for (i = 0; i < 3; ++i) {
        if (count > (size_t)-1 - sizes[i])
            goto overflow;
        count += sizes[i];
        for (j = 0; j < sizes[i]; ++j) {
            size_t a = groups[i][j].name.length, b = groups[i][j].value.length;
            if (a > (size_t)-1 - 2 || b > (size_t)-1 - a - 2 ||
                total > (size_t)-1 - a - b - 2)
                goto overflow;
            total += a + b + 2;
        }
    }
    if (count > (size_t)-1 / sizeof(**pairs))
        goto overflow;
    *text = malloc(total);
    *pairs = count != 0 ? malloc(count * sizeof(**pairs)) : NULL;
    if (*text == NULL || (count != 0 && *pairs == NULL)) {
        free(*text);
        free(*pairs);
        *text = NULL;
        *pairs = NULL;
        return rotmos_fail(error, ROTMOS_NOMEM, "upload_metadata",
                           "copying request metadata");
    }
    cursor = *text;
    for (i = 0; i < 11; ++i) {
        *output[i] = input[i];
        output[i]->data = cursor;
        if (input[i].length != 0)
            memcpy(cursor, input[i].data, input[i].length);
        cursor[input[i].length] = 0;
        cursor += input[i].length + 1;
    }
    p = *pairs;
    m->headers = p;
    m->header_count = sizes[0];
    m->queries = sizes[1] ? p + sizes[0] : NULL;
    m->query_count = sizes[1];
    m->params = sizes[2] ? p + sizes[0] + sizes[1] : NULL;
    m->param_count = sizes[2];
    for (i = 0; i < 3; ++i)
        for (j = 0; j < sizes[i]; ++j, ++p) {
            p->name = groups[i][j].name;
            p->name.data = cursor;
            if (p->name.length != 0)
                memcpy(cursor, groups[i][j].name.data, p->name.length);
            cursor[p->name.length] = 0;
            cursor += p->name.length + 1;
            p->value = groups[i][j].value;
            p->value.data = cursor;
            if (p->value.length != 0)
                memcpy(cursor, groups[i][j].value.data, p->value.length);
            cursor[p->value.length] = 0;
            cursor += p->value.length + 1;
        }
    m->method = r->method;
    m->peer.port = r->peer.port;
    m->local.port = r->local.port;
    m->listener_index = r->listener;
    m->tls = r->tls;
    m->content_length = r->content_length;
    m->has_content_length = r->has_length;
    return ROTMOS_OK;
overflow:
    return rotmos_fail(error, ROTMOS_OVERFLOW, "upload_metadata",
                       "metadata size overflow");
}
