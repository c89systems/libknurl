#ifndef ROTMOS_ACME_INTERNAL_H
#define ROTMOS_ACME_INTERNAL_H
#include "tls_internal.h"
int rotmos_acme_domain_valid(const char *domain);
rotmos_status rotmos_acme_runtime_directory(const rotmos_tls_config *, char **,
                                            rotmos_error *);
rotmos_status rotmos_acme_state_call(const rotmos_tls_config *, rotmos_server *,
                                     const rotmos_process_context *,
                                     rotmos_acme_event, const char *domain,
                                     const char *directory, rotmos_error *);
#endif
