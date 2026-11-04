#ifndef ROTMOS_FRAMING_H
#define ROTMOS_FRAMING_H
#include <rotmos/common.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef rotmos_status (*rotmos_framer_body_fn)(rotmos_bytes, size_t *, void *,
                                               rotmos_error *);
typedef rotmos_status (*rotmos_framer_trailer_fn)(rotmos_header, void *,
                                                  rotmos_error *);
typedef struct rotmos_framer_config {
    int chunked;
    rotmos_u64 content_length;
    /* Zero permits any checked 64-bit body length; storage stays bounded. */
    rotmos_u64 max_body_bytes;
    rotmos_framer_body_fn on_body;
    rotmos_framer_trailer_fn on_trailer;
    void *userdata;
} rotmos_framer_config;

/* Independent incremental HTTP body decoder. No transport, threads or body
 * storage. Chunk lines <=128 bytes, trailer lines <=8192, block <=16384 and
 * fields <=100. Transfer-coding/length header validation precedes construction.
 * Configuration is copied; application userdata remains caller-owned. */
ROTMOS_API void rotmos_framer_config_init(rotmos_framer_config *);
ROTMOS_API rotmos_status rotmos_framer_new(const rotmos_framer_config *,
                                           rotmos_framer **, rotmos_error *);
/* Consumes only this message. EOF is its complete boundary, leaving pipeline
 * or tunnel suffix bytes untouched. AGAIN reports the accepted prefix only.
 * Retain the suffix until capacity returns; no implicit retries or callbacks.
 * Body OK accepts a positive bounded prefix; AGAIN accepts zero. Trailer
 * callbacks are synchronous OK/error operations. Errors are sticky. */
ROTMOS_API rotmos_status rotmos_framer_feed(rotmos_framer *, const void *,
                                            size_t, size_t *, rotmos_error *);
/* Declare transport EOF. An incomplete message fails with INVALID. */
ROTMOS_API rotmos_status rotmos_framer_finish(rotmos_framer *, rotmos_error *);
ROTMOS_API rotmos_u64 rotmos_framer_body_bytes(const rotmos_framer *);
/* Returns STATE if called from a framer callback. */
ROTMOS_API rotmos_status rotmos_framer_destroy(rotmos_framer *, rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
