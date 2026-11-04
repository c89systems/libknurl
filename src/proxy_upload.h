#ifndef ROTMOS_PROXY_UPLOAD_H
#define ROTMOS_PROXY_UPLOAD_H

#include "proxy_framing.h"
#include "proxy_headers.h"

#include <curl/curl.h>
#include <rotmos/rotmos.h>

/* One bounded request-body chunk and its incremental framing state. No body
 * byte is consumed unless it fits this queue. Request trailers are retained
 * separately under the framer's bounded trailer limit. */
typedef struct rotmos_proxy_upload_buffer {
    rotmos_proxy_body_framer framer;
    rotmos_proxy_headers declared;
    rotmos_proxy_headers trailers;
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
    rotmos_status failure;
    void (*on_consume)(void *userdata, size_t amount);
    void *consume_userdata;
    int complete;
    int curl_paused;
} rotmos_proxy_upload_buffer;

rotmos_status
rotmos_proxy_upload_init(rotmos_proxy_upload_buffer *upload, size_t capacity,
                         int chunked, uint64_t content_length,
                         const rotmos_proxy_headers *request_headers,
                         rotmos_error *error);
void rotmos_proxy_upload_cleanup(rotmos_proxy_upload_buffer *upload);

/* consumed reports exactly the prefix accepted from this input span. A
 * PAUSED result leaves the remaining bytes with the caller until resume. */
rotmos_proxy_frame_result
rotmos_proxy_upload_feed(rotmos_proxy_upload_buffer *upload,
                         const unsigned char *data, size_t length,
                         size_t *consumed);

/* Fixed-length uploads may receive directly into the queue. Reserve returns
 * space no larger than the remaining body; commit publishes received bytes. */
unsigned char *
rotmos_proxy_upload_reserve_fixed(rotmos_proxy_upload_buffer *upload,
                                  size_t *capacity);
rotmos_proxy_frame_result
rotmos_proxy_upload_commit_fixed(rotmos_proxy_upload_buffer *upload,
                                 size_t length);

/* libcurl read callback. Returns CURL_READFUNC_PAUSE while waiting for input,
 * and zero only after the framer has completed and the queue has drained. */
size_t rotmos_proxy_upload_read(char *buffer, size_t size, size_t count,
                                void *userdata);
int rotmos_proxy_upload_full(const rotmos_proxy_upload_buffer *upload);
int rotmos_proxy_upload_can_resume(const rotmos_proxy_upload_buffer *upload);
void rotmos_proxy_upload_resumed(rotmos_proxy_upload_buffer *upload);

/* Called by libcurl only after upload_read has returned EOF. The returned
 * list is owned and freed by libcurl. */
int rotmos_proxy_upload_curl_trailers(struct curl_slist **list, void *userdata);

#endif
