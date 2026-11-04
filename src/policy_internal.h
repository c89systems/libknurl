#ifndef ROTMOS_POLICY_INTERNAL_H
#define ROTMOS_POLICY_INTERNAL_H
#include <rotmos/server.h>
#include "request_internal.h"
rotmos_status rotmos_request_identity(rotmos_request *, rotmos_error *);
rotmos_status rotmos_policy_start(rotmos_request *, rotmos_error *);
rotmos_status rotmos_policy_admit(rotmos_request *, int *, rotmos_error *);
void rotmos_policy_observe(rotmos_request *, rotmos_observation_kind,
                           const char *, int, rotmos_status);
void rotmos_policy_closed(rotmos_request *);
#endif
