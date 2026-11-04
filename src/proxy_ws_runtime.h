#ifndef ROTMOS_PROXY_WS_RUNTIME_H
#define ROTMOS_PROXY_WS_RUNTIME_H
#include "proxy_headers.h"
#include <rotmos/common.h>
struct proxy_exchange;
rotmos_status rotmos_proxy_ws_start(struct proxy_exchange *, const char *,
                                    const rotmos_proxy_headers *,
                                    rotmos_proxy_headers *, rotmos_error *);
rotmos_status rotmos_proxy_ws_step(struct proxy_exchange *);
int rotmos_proxy_ws_tunneled(const struct proxy_exchange *);
void rotmos_proxy_ws_stop(struct proxy_exchange *);
void rotmos_proxy_ws_clear(struct proxy_exchange *);
#endif
