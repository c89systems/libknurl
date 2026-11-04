#ifndef ROTMOS_UPLOAD_INTERNAL_H
#define ROTMOS_UPLOAD_INTERNAL_H
#include <rotmos/upload.h>
#include "request_internal.h"
#include "response_internal.h"
rotmos_status rotmos_upload_metadata_copy(rotmos_request *,
                                          rotmos_upload_metadata *, void **text,
                                          rotmos_header **pairs,
                                          rotmos_error *);
rotmos_status rotmos_upload_reader_add(rotmos_router *, rotmos_route_group *,
                                       const rotmos_upload_reader_config *,
                                       void (*release_userdata)(void *),
                                       rotmos_route **, rotmos_error *);
#endif
