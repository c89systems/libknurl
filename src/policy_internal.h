#ifndef KNURL_POLICY_INTERNAL_H
#define KNURL_POLICY_INTERNAL_H
#include <knurl/server.h>
#include "request_internal.h"
knurl_status knurl_request_identity(knurl_request *, knurl_error *);
void knurl_request_autoblock_context(knurl_request *, knurl_autoblock_event *);
knurl_status knurl_policy_start(knurl_request *, knurl_error *);
knurl_status knurl_policy_admit(knurl_request *, int *, knurl_error *);
void knurl_policy_observe(knurl_request *, knurl_observation_kind, const char *,
                          int, knurl_status);
void knurl_policy_closed(knurl_request *);
#endif
