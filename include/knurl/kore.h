#ifndef KNURL_KORE_H
#define KNURL_KORE_H

/** @file kore.h Public kore API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

struct http_request;
/** Return the packaged Kore commit string in static storage. */
KNURL_API const char *knurl_kore_revision(const knurl_server *server);
/** Borrowed native view, tied to this packaged backend revision. No native
 * transport intervention is permitted while managed I/O owns the connection. */
KNURL_API struct http_request *knurl_kore_request(knurl_request *request);

#ifdef __cplusplus
}
#endif
#endif
