#ifndef KNURL_JSON_INTERNAL_H
#define KNURL_JSON_INTERNAL_H
#include <knurl/lonejson.h>
knurl_status knurl_json_schema_check(const lonejson_schema_view *,
                                     knurl_error *);
knurl_status knurl_json_record_check(const lonejson_schema_view *,
                                     const lonejson_record_view *,
                                     knurl_error *);
knurl_status knurl_json_error(lonejson_status, const lonejson_error *,
                              knurl_error *);
knurl_status knurl_json_runtime(const lonejson_schema_view *, lonejson **,
                                int *, knurl_error *);
struct knurl_json_buffer {
    knurl_owned_bytes bytes;
    size_t capacity;
    size_t bound;
    knurl_status failure;
};
lonejson_status knurl_json_buffer_write(void *, const void *, size_t,
                                        lonejson_error *);
#endif
