#ifndef KNURL_UPLOAD_INTERNAL_H
#define KNURL_UPLOAD_INTERNAL_H
#include <knurl/upload.h>
#include "request_internal.h"
#include "response_internal.h"
/* Private parser bridge over the existing reader queue. Preparation and finish
 * run on the owner loop. Only its independently retained state enters native
 * parsing; release waits for both request cleanup and native acknowledgement.
 */
struct knurl_upload_reader_codec {
    knurl_status (*open)(knurl_request *, void *, void **, knurl_error *);
    knurl_status (*finish)(knurl_request *, knurl_reply *, void *,
                           knurl_error *);
    void (*release)(void *);
};
struct knurl_native_metadata {
    knurl_upload_metadata view;
    knurl_auth_identity identity;
};
knurl_status knurl_upload_metadata_copy(knurl_request *,
                                        struct knurl_native_metadata *,
                                        void **text, knurl_header **pairs,
                                        knurl_error *);
knurl_status knurl_blocking_pool_start(knurl_server *, knurl_error *);
knurl_status knurl_upload_reader_add(knurl_router *, knurl_route_group *,
                                     const knurl_upload_reader_config *,
                                     const struct knurl_upload_reader_codec *,
                                     void (*release_userdata)(void *),
                                     knurl_route **, knurl_error *);
#endif
