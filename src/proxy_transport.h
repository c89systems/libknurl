#ifndef KNURL_PROXY_TRANSPORT_H
#define KNURL_PROXY_TRANSPORT_H
#include <knurl/loop.h>
#include <curl/curl.h>
typedef struct knurl_proxy_transport knurl_proxy_transport;
typedef struct knurl_proxy_transfer knurl_proxy_transfer;
typedef void (*knurl_proxy_done_fn)(CURL *, CURLcode, void *);
/* Worker/process-local owner-loop pool. Zero limits select 16 active exchanges
 * shared across both pools and four retained idle connections per pool. */
knurl_status knurl_proxy_transport_new(knurl_loop *, size_t, size_t,
                                       knurl_proxy_transport **, knurl_error *);
knurl_status knurl_proxy_transport_destroy(knurl_proxy_transport *,
                                           knurl_error *);
/* Success takes easy, failure leaves it with caller. Done borrows easy for its
 * invocation. Successful connect-only completion retains the transfer until
 * cancel, including its admission slot. Cancel never calls done. */
knurl_status knurl_proxy_transport_submit(knurl_proxy_transport *, CURL *, int,
                                          int, knurl_proxy_done_fn, void *,
                                          knurl_proxy_transfer **,
                                          knurl_error *);
void knurl_proxy_transfer_cancel(knurl_proxy_transfer *);
knurl_status knurl_proxy_transfer_handoff(knurl_proxy_transfer *,
                                          curl_socket_t *, knurl_error *);
size_t knurl_proxy_transport_active(const knurl_proxy_transport *);
knurl_status knurl_proxy_curl_set_ca(CURL *, char *, size_t, knurl_error *);
knurl_status knurl_proxy_curl_set_client_identity(CURL *, char *, size_t,
                                                  char *, size_t,
                                                  knurl_error *);
int knurl_proxy_curl_configure_http(CURL *, int);
#endif
