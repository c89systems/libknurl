#ifndef KNURL_PROXY_URL_H
#define KNURL_PROXY_URL_H

#include <knurl/knurl.h>

/* Build an origin-form target from a configured base URL and raw inbound
 * components. Both outputs are owned by the caller; neither holds body bytes.
 * raw_query excludes the leading '?'. */
knurl_status knurl_proxy_target_build(const char *base_url,
                                      const char *raw_path,
                                      const char *raw_query,
                                      char **request_target, char **authority,
                                      knurl_error *error);

#endif
