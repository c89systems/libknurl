#ifndef KNURL_PROXY_WS_RUNTIME_H
#define KNURL_PROXY_WS_RUNTIME_H
#include "proxy_headers.h"
#include <knurl/common.h>
struct proxy_exchange;
knurl_status knurl_proxy_ws_start(struct proxy_exchange *, const char *,
                                  const knurl_proxy_headers *,
                                  knurl_proxy_headers *, knurl_error *);
knurl_status knurl_proxy_ws_step(struct proxy_exchange *);
int knurl_proxy_ws_tunneled(const struct proxy_exchange *);
void knurl_proxy_ws_stop(struct proxy_exchange *);
void knurl_proxy_ws_clear(struct proxy_exchange *);
#endif
