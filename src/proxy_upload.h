#ifndef KNURL_PROXY_UPLOAD_H
#define KNURL_PROXY_UPLOAD_H

#include "proxy_framing.h"
#include "proxy_headers.h"

#include <curl/curl.h>
#include <knurl/knurl.h>

/* One bounded request-body chunk and its incremental framing state. No body
 * byte is consumed unless it fits this queue. Request trailers are retained
 * separately under the framer's bounded trailer limit. */
typedef struct knurl_proxy_upload_buffer {
    knurl_proxy_body_framer framer;
    knurl_proxy_headers declared;
    knurl_proxy_headers trailers;
    unsigned char *bytes;
    size_t capacity;
    size_t begin;
    size_t end;
    size_t fixed_reservation;
    /* Set once after init and before feed. Zero permits arbitrary checked
     * lengths; this is the server's explicit hard limit, not its soft default.
     */
    uint64_t body_limit;
    uint64_t body_accepted;
    knurl_status failure;
    void (*on_consume)(void *userdata, size_t amount);
    void *consume_userdata;
    int complete;
    int curl_paused;
} knurl_proxy_upload_buffer;

knurl_status knurl_proxy_upload_init(knurl_proxy_upload_buffer *upload,
                                     size_t capacity, int chunked,
                                     uint64_t content_length,
                                     const knurl_proxy_headers *request_headers,
                                     knurl_error *error);
void knurl_proxy_upload_cleanup(knurl_proxy_upload_buffer *upload);

/* consumed reports exactly the prefix accepted from this input span. A
 * PAUSED result leaves the remaining bytes with the caller until resume. */
knurl_proxy_frame_result
knurl_proxy_upload_feed(knurl_proxy_upload_buffer *upload,
                        const unsigned char *data, size_t length,
                        size_t *consumed);

/* Fixed-length uploads may receive directly into the queue. Reserve returns
 * space no larger than the remaining body; commit publishes received bytes. */
unsigned char *
knurl_proxy_upload_reserve_fixed(knurl_proxy_upload_buffer *upload,
                                 size_t *capacity);
knurl_proxy_frame_result
knurl_proxy_upload_commit_fixed(knurl_proxy_upload_buffer *upload,
                                size_t length);

/* libcurl read callback. Returns CURL_READFUNC_PAUSE while waiting for input,
 * and zero only after the framer has completed and the queue has drained. */
size_t knurl_proxy_upload_read(char *buffer, size_t size, size_t count,
                               void *userdata);
int knurl_proxy_upload_full(const knurl_proxy_upload_buffer *upload);
int knurl_proxy_upload_can_resume(const knurl_proxy_upload_buffer *upload);
void knurl_proxy_upload_resumed(knurl_proxy_upload_buffer *upload);

/* Called by libcurl only after upload_read has returned EOF. The returned
 * list is owned and freed by libcurl. */
int knurl_proxy_upload_curl_trailers(struct curl_slist **list, void *userdata);

#endif
