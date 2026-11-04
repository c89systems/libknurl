#ifndef ROTMOS_PROXY_TRANSPORT_H
#define ROTMOS_PROXY_TRANSPORT_H
#include <rotmos/loop.h>
#include <curl/curl.h>
typedef struct rotmos_proxy_transport rotmos_proxy_transport;
typedef struct rotmos_proxy_transfer rotmos_proxy_transfer;
typedef void (*rotmos_proxy_done_fn)(CURL *, CURLcode, void *);
/* Worker/process-local owner-loop pool. Zero limits select 16 active exchanges
 * shared across both pools and four retained idle connections per pool. */
rotmos_status rotmos_proxy_transport_new(rotmos_loop *, size_t, size_t,
                                         rotmos_proxy_transport **,
                                         rotmos_error *);
rotmos_status rotmos_proxy_transport_destroy(rotmos_proxy_transport *,
                                             rotmos_error *);
/* Success takes easy, failure leaves it with caller. Done borrows easy for its
 * invocation. Successful connect-only completion retains the transfer until
 * cancel, including its admission slot. Cancel never calls done. */
rotmos_status rotmos_proxy_transport_submit(rotmos_proxy_transport *, CURL *,
                                            int, int, rotmos_proxy_done_fn,
                                            void *, rotmos_proxy_transfer **,
                                            rotmos_error *);
void rotmos_proxy_transfer_cancel(rotmos_proxy_transfer *);
rotmos_status rotmos_proxy_transfer_handoff(rotmos_proxy_transfer *,
                                            curl_socket_t *, rotmos_error *);
size_t rotmos_proxy_transport_active(const rotmos_proxy_transport *);
rotmos_status rotmos_proxy_curl_set_ca(CURL *, char *, size_t, rotmos_error *);
rotmos_status rotmos_proxy_curl_set_client_identity(CURL *, char *, size_t,
                                                    char *, size_t,
                                                    rotmos_error *);
int rotmos_proxy_curl_configure_http(CURL *, int);
#endif
