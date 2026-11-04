#ifndef ROTMOS_COMMON_H
#define ROTMOS_COMMON_H

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define ROTMOS_API __attribute__((visibility("default")))
#else
#define ROTMOS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rotmos_server rotmos_server;
typedef struct rotmos_process_context rotmos_process_context;
typedef struct rotmos_router rotmos_router;
typedef struct rotmos_route rotmos_route;
typedef struct rotmos_route_group rotmos_route_group;
typedef struct rotmos_request rotmos_request;
typedef struct rotmos_request_plan rotmos_request_plan;
typedef struct rotmos_body rotmos_body;
typedef struct rotmos_channel rotmos_channel;
typedef struct rotmos_websocket rotmos_websocket;
typedef struct rotmos_framer rotmos_framer;
typedef struct rotmos_response rotmos_response;
typedef struct rotmos_reply rotmos_reply;
typedef struct rotmos_source rotmos_source;
typedef struct rotmos_loop rotmos_loop;
typedef struct rotmos_watch rotmos_watch;
typedef struct rotmos_timer rotmos_timer;
typedef struct rotmos_waker rotmos_waker;

typedef enum rotmos_status {
    ROTMOS_OK = 0,
    ROTMOS_AGAIN,
    ROTMOS_EOF,
    ROTMOS_INVALID,
    ROTMOS_NOMEM,
    ROTMOS_STATE,
    ROTMOS_CONFLICT,
    ROTMOS_LIMIT,
    ROTMOS_OVERFLOW,
    ROTMOS_TIMEOUT,
    ROTMOS_IO,
    ROTMOS_NOT_SUPPORTED,
    ROTMOS_CANCELLED,
    ROTMOS_BACKEND
} rotmos_status;

typedef enum rotmos_error_source {
    ROTMOS_ERROR_NONE = 0,
    ROTMOS_ERROR_CORE,
    ROTMOS_ERROR_SYSTEM,
    ROTMOS_ERROR_BACKEND,
    ROTMOS_ERROR_LONEJSON,
    ROTMOS_ERROR_PSLOG,
    ROTMOS_ERROR_XML
} rotmos_error_source;

typedef struct rotmos_error {
    rotmos_status status;
    rotmos_error_source source;
    long native_code;
    char operation[48];
    char message[192];
    char detail[128];
} rotmos_error;

typedef struct rotmos_bytes {
    const void *data;
    size_t length;
} rotmos_bytes;

typedef struct rotmos_header {
    rotmos_bytes name;
    rotmos_bytes value;
} rotmos_header;

/* Each member is a 32-bit word, including when unsigned long is wider. */
typedef struct rotmos_u64 {
    unsigned long high;
    unsigned long low;
} rotmos_u64;

ROTMOS_API const char *rotmos_version(void);
ROTMOS_API void rotmos_error_clear(rotmos_error *error);
ROTMOS_API const char *rotmos_status_string(rotmos_status status);
ROTMOS_API rotmos_status rotmos_u64_from_words(unsigned long high,
                                               unsigned long low,
                                               rotmos_u64 *out,
                                               rotmos_error *error);
ROTMOS_API rotmos_status rotmos_u64_from_size(size_t value, rotmos_u64 *out,
                                              rotmos_error *error);
ROTMOS_API rotmos_status rotmos_u64_to_size(rotmos_u64 value, size_t *out,
                                            rotmos_error *error);
ROTMOS_API rotmos_status rotmos_u64_compare(rotmos_u64 left, rotmos_u64 right,
                                            int *order, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_u64_add(rotmos_u64 left, rotmos_u64 right,
                                        rotmos_u64 *out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_u64_from_decimal(const char *text,
                                                 size_t length, rotmos_u64 *out,
                                                 rotmos_error *error);
/* The destination includes a trailing NUL. Failure leaves it untouched. */
ROTMOS_API rotmos_status rotmos_u64_format_decimal(rotmos_u64 value,
                                                   char *buffer,
                                                   size_t capacity,
                                                   size_t *written,
                                                   rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
