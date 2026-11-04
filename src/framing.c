#include "proxy_framing.h"
#include "internal.h"
#include <rotmos/framing.h>
#include <stdlib.h>
#include <string.h>

struct rotmos_framer {
    rotmos_proxy_body_framer frame;
    rotmos_framer_config config;
    rotmos_u64 delivered;
    rotmos_error failure;
    int busy;
};

void rotmos_framer_config_init(rotmos_framer_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

static rotmos_status fail(rotmos_framer *f, rotmos_status cause,
                          const char *message)
{
    return rotmos_fail(&f->failure, cause, "framer", message);
}

rotmos_status rotmos_framer_new(const rotmos_framer_config *config,
                                rotmos_framer **out, rotmos_error *error)
{
    rotmos_framer *f;
    uint64_t length;
    int order;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->on_body == NULL ||
        (config->chunked != 0 && config->chunked != 1) ||
        !rotmos_u64_valid(config->content_length) ||
        !rotmos_u64_valid(config->max_body_bytes) ||
        (config->chunked &&
         (config->content_length.high || config->content_length.low)))
        return rotmos_fail(
            error, ROTMOS_INVALID, "framer_new",
            "provide framing, checked lengths, body callback and output");
    if (!config->chunked &&
        (config->max_body_bytes.high || config->max_body_bytes.low)) {
        (void)rotmos_u64_compare(config->content_length, config->max_body_bytes,
                                 &order, NULL);
        if (order > 0)
            return rotmos_fail(error, ROTMOS_LIMIT, "framer_new",
                               "declared body exceeds limit");
    }
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "framer_new",
                           "cannot allocate bounded decoder");
    f->config = *config;
    length = ((uint64_t)config->content_length.high << 32) |
             config->content_length.low;
    if (config->chunked)
        rotmos_proxy_body_framer_chunked(&f->frame);
    else
        rotmos_proxy_body_framer_fixed(&f->frame, length);
    *out = f;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static size_t body(void *data, const unsigned char *bytes, size_t length)
{
    rotmos_framer *f;
    rotmos_bytes view;
    rotmos_u64 amount;
    rotmos_u64 projected;
    rotmos_status status;
    rotmos_error error;
    size_t consumed;
    int order;
    f = data;
    if (rotmos_u64_from_size(length, &amount, NULL) != ROTMOS_OK ||
        rotmos_u64_add(f->delivered, amount, &projected, NULL) != ROTMOS_OK) {
        (void)fail(f, ROTMOS_OVERFLOW, "body counter overflow");
        return 0;
    }
    if (f->config.max_body_bytes.high || f->config.max_body_bytes.low) {
        (void)rotmos_u64_compare(projected, f->config.max_body_bytes, &order,
                                 NULL);
        if (order > 0) {
            (void)fail(f, ROTMOS_LIMIT, "body exceeds configured limit");
            return 0;
        }
    }
    view.data = bytes;
    view.length = length;
    consumed = 0;
    rotmos_error_clear(&error);
    status = f->config.on_body(view, &consumed, f->config.userdata, &error);
    if ((status == ROTMOS_OK && (consumed == 0 || consumed > length)) ||
        (status != ROTMOS_OK && consumed != 0) || status == ROTMOS_EOF ||
        status < ROTMOS_OK || status > ROTMOS_BACKEND) {
        (void)fail(f, ROTMOS_INVALID,
                   "body callback returned invalid progress");
        return 0;
    }
    if (status != ROTMOS_OK && status != ROTMOS_AGAIN) {
        if (error.status == status)
            f->failure = error;
        else
            (void)fail(f, status, "body callback failed");
        return 0;
    }
    (void)rotmos_u64_from_size(consumed, &amount, NULL);
    (void)rotmos_u64_add(f->delivered, amount, &f->delivered, NULL);
    return consumed;
}

static int trailer(void *data, const char *name, const char *value)
{
    rotmos_framer *f;
    rotmos_header header;
    rotmos_status status;
    rotmos_error error;
    f = data;
    if (f->config.on_trailer == NULL)
        return 1;
    header.name.data = name;
    header.name.length = strlen(name);
    header.value.data = value;
    header.value.length = strlen(value);
    rotmos_error_clear(&error);
    status = f->config.on_trailer(header, f->config.userdata, &error);
    if (status == ROTMOS_OK)
        return 1;
    if (status <= ROTMOS_EOF || status > ROTMOS_BACKEND)
        status = ROTMOS_INVALID;
    if (error.status == status)
        f->failure = error;
    else
        (void)fail(f, status, "trailer callback failed");
    return 0;
}

static rotmos_status result(rotmos_framer *f, rotmos_error *error)
{
    if (f->failure.status != ROTMOS_OK) {
        f->frame.phase = ROTMOS_PROXY_PHASE_INVALID;
        if (error != NULL)
            *error = f->failure;
        return f->failure.status;
    }
    rotmos_error_clear(error);
    return f->frame.phase == ROTMOS_PROXY_PHASE_COMPLETE ? ROTMOS_EOF
                                                         : ROTMOS_OK;
}

rotmos_status rotmos_framer_feed(rotmos_framer *f, const void *data,
                                 size_t length, size_t *consumed,
                                 rotmos_error *error)
{
    rotmos_proxy_frame_result state;
    rotmos_status status;
    if (consumed != NULL)
        *consumed = 0;
    if (f == NULL || consumed == NULL || (data == NULL && length != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "framer_feed",
                           "provide decoder, bytes and count");
    if (f->busy)
        return rotmos_fail(error, ROTMOS_STATE, "framer_feed",
                           "decoder callback is active");
    status = result(f, error);
    if (status != ROTMOS_OK)
        return status;
    f->busy = 1;
    state = rotmos_proxy_body_framer_feed(&f->frame, data, length, consumed,
                                          body, trailer, f);
    f->busy = 0;
    if (state == ROTMOS_PROXY_FRAME_INVALID && f->failure.status == ROTMOS_OK)
        (void)fail(f, ROTMOS_INVALID,
                   "invalid or oversized HTTP chunk/trailer framing");
    status = result(f, error);
    return status == ROTMOS_OK && state == ROTMOS_PROXY_FRAME_PAUSED
               ? ROTMOS_AGAIN
               : status;
}

rotmos_status rotmos_framer_finish(rotmos_framer *f, rotmos_error *error)
{
    if (f == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "framer_finish",
                           "provide decoder");
    if (f->busy)
        return rotmos_fail(error, ROTMOS_STATE, "framer_finish",
                           "decoder callback is active");
    if (f->frame.phase != ROTMOS_PROXY_PHASE_COMPLETE &&
        f->failure.status == ROTMOS_OK)
        (void)fail(f, ROTMOS_INVALID,
                   "transport EOF before HTTP body boundary");
    return result(f, error);
}

rotmos_u64 rotmos_framer_body_bytes(const rotmos_framer *f)
{
    rotmos_u64 zero = {0, 0};
    return f != NULL ? f->delivered : zero;
}

rotmos_status rotmos_framer_destroy(rotmos_framer *f, rotmos_error *error)
{
    if (f != NULL && f->busy)
        return rotmos_fail(error, ROTMOS_STATE, "framer_destroy",
                           "decoder callback is active");
    free(f);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
