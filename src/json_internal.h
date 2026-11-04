#ifndef ROTMOS_JSON_INTERNAL_H
#define ROTMOS_JSON_INTERNAL_H
#include <rotmos/lonejson.h>
rotmos_status rotmos_json_schema_check(const lonejson_schema_view *,
                                       rotmos_error *);
rotmos_status rotmos_json_error(lonejson_status, const lonejson_error *,
                                rotmos_error *);
rotmos_status rotmos_json_runtime(const lonejson_schema_view *, lonejson **,
                                  int *, rotmos_error *);
struct rotmos_json_buffer {
    rotmos_owned_bytes bytes;
    size_t capacity;
    size_t bound;
    rotmos_status failure;
};
lonejson_status rotmos_json_buffer_write(void *, const void *, size_t,
                                         lonejson_error *);
#endif
