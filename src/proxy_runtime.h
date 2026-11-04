#ifndef ROTMOS_PROXY_RUNTIME_H
#define ROTMOS_PROXY_RUNTIME_H
#include <rotmos/dispatch.h>
rotmos_status rotmos_proxy_dispatch(rotmos_request *, rotmos_request_plan *,
                                    void *, rotmos_error *);
void rotmos_proxy_worker_clear(rotmos_server *);
#endif
