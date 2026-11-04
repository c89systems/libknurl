#ifndef ROTMOS_PROXY_URL_H
#define ROTMOS_PROXY_URL_H

#include <rotmos/rotmos.h>

/* Build an origin-form target from a configured base URL and raw inbound
 * components. Both outputs are owned by the caller; neither holds body bytes.
 * raw_query excludes the leading '?'. */
rotmos_status rotmos_proxy_target_build(const char *base_url,
                                        const char *raw_path,
                                        const char *raw_query,
                                        char **request_target, char **authority,
                                        rotmos_error *error);

#endif
