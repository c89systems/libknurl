#ifndef KNURL_ACME_INTERNAL_H
#define KNURL_ACME_INTERNAL_H
#include "tls_internal.h"
int knurl_acme_domain_valid(const char *domain);
knurl_status knurl_acme_runtime_directory(const knurl_tls_config *, char **,
                                          knurl_error *);
knurl_status knurl_acme_state_call(const knurl_tls_config *, knurl_server *,
                                   const knurl_process_context *,
                                   knurl_acme_event, const char *domain,
                                   const char *directory, knurl_error *);
#endif
