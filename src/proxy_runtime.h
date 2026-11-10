#ifndef KNURL_PROXY_RUNTIME_H
#define KNURL_PROXY_RUNTIME_H
#include <knurl/dispatch.h>
knurl_status knurl_proxy_dispatch(knurl_request *, knurl_request_plan *, void *,
                                  knurl_error *);
void knurl_proxy_worker_clear(knurl_server *);
#endif
