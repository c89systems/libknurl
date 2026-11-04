#ifndef ROTMOS_JSON_INTERNAL_H
#define ROTMOS_JSON_INTERNAL_H
#include <rotmos/lonejson.h>
rotmos_status rotmos_json_schema_check(const lonejson_schema_view *,
                                       rotmos_error *);
rotmos_status rotmos_json_error(lonejson_status, const lonejson_error *,
                                rotmos_error *);
rotmos_status rotmos_json_runtime(const lonejson_schema_view *, lonejson **,
                                  int *, rotmos_error *);
#endif
