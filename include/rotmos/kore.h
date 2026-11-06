#ifndef ROTMOS_KORE_H
#define ROTMOS_KORE_H

/** @file kore.h Public kore API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

struct http_request;
/** Return the packaged Kore commit string in static storage. */
ROTMOS_API const char *rotmos_kore_revision(const rotmos_server *server);
/** Borrowed native view, tied to this packaged backend revision. No native
 * transport intervention is permitted while managed I/O owns the connection. */
ROTMOS_API struct http_request *rotmos_kore_request(rotmos_request *request);

#ifdef __cplusplus
}
#endif
#endif
