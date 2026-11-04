#ifndef ROTMOS_UPLOAD_INTERNAL_H
#define ROTMOS_UPLOAD_INTERNAL_H
#include <rotmos/upload.h>
#include "request_internal.h"
#include "response_internal.h"
/* Private parser bridge over the existing reader queue. Preparation and finish
 * run on the owner loop. Only its independently retained state enters native
 * parsing; release waits for both request cleanup and native acknowledgement.
 */
struct rotmos_upload_reader_codec {
    rotmos_status (*open)(rotmos_request *, void *, void **, rotmos_error *);
    rotmos_status (*finish)(rotmos_request *, rotmos_reply *, void *,
                            rotmos_error *);
    void (*release)(void *);
};
rotmos_status rotmos_upload_metadata_copy(rotmos_request *,
                                          rotmos_upload_metadata *, void **text,
                                          rotmos_header **pairs,
                                          rotmos_error *);
rotmos_status rotmos_upload_reader_add(
    rotmos_router *, rotmos_route_group *, const rotmos_upload_reader_config *,
    const struct rotmos_upload_reader_codec *, void (*release_userdata)(void *),
    rotmos_route **, rotmos_error *);
#endif
