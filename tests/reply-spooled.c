#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "spooled %d: %s\n", __LINE__, #t);                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct input {
    knurl_reply *reply;
    knurl_source *source;
    size_t total, remaining;
    int mode, reads, resets, closes;
    knurl_status reenter_reply, reenter_source, outcome;
};
static knurl_status failure(knurl_error *error, const char *message)
{
    knurl_error_clear(error);
    if (error != NULL) {
        error->status = KNURL_IO;
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = 321;
        snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return KNURL_IO;
}
static knurl_status reset(void *data, knurl_error *error)
{
    struct input *s = data;
    ++s->resets;
    s->remaining = s->total;
    return s->mode == 2 ? failure(error, "reset-first") : KNURL_OK;
}
static knurl_status read_input(void *buffer, size_t capacity, size_t *produced,
                               void *data, knurl_error *error)
{
    struct input *s = data;
    ++s->reads;
    s->reenter_source = knurl_source_destroy(s->source, NULL);
    s->reenter_reply = knurl_reply_destroy(s->reply, NULL);
    *produced = 0;
    if (s->mode == 1 && s->reads == 2)
        return KNURL_AGAIN;
    if (s->mode == 3 && s->reads == 2)
        return failure(error, "read-first");
    if (!s->remaining)
        return KNURL_EOF;
    *produced = s->remaining < capacity ? s->remaining : capacity;
    memset(buffer, 'q', *produced);
    s->remaining -= *produced;
    return KNURL_OK;
}
static void close_input(void *data, knurl_status outcome)
{
    struct input *s = data;
    ++s->closes;
    s->outcome = outcome;
}
static int next_fd(void)
{
    int fd = fcntl(STDOUT_FILENO, F_DUPFD, 3);
    if (fd >= 0)
        close(fd);
    return fd;
}
struct closing {
    knurl_source *source;
    knurl_status result;
    int closed;
};
static knurl_status empty(void *buffer, size_t capacity, size_t *count,
                          void *data, knurl_error *error)
{
    (void)buffer;
    (void)capacity;
    (void)data;
    (void)error;
    *count = 0;
    return KNURL_EOF;
}
static void close_previous(void *data, knurl_status cause)
{
    struct closing *s = data;
    (void)cause;
    ++s->closed;
    s->result = knurl_source_destroy(s->source, NULL);
}
struct record {
    char *message;
};
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(struct record, message, "message")};
LONEJSON_MAP_DEFINE(map, struct record, fields);
int main(void)
{
    knurl_reply *reply;
    knurl_source *previous;
    knurl_source_config config;
    knurl_source_materialize_config materialize;
    knurl_error error;
    struct input input;
    struct closing closing;
    struct stat info;
    struct rlimit saved, limited;
    void (*handler)(int);
    knurl_status status;
    lonejson_schema_view schema;
    struct record record;
    char bytes[64], expected[] = "{\"message\":\"before\"}";
    int fd, mode;
    CHECK(knurl_reply_new(&reply, &error) == KNURL_OK);
    CHECK(knurl_reply_header_add(reply, "X-Test", "retained", &error) ==
          KNURL_OK);
    memset(&input, 0, sizeof(input));
    input.reply = reply;
    input.total = 10485760UL + 13;
    knurl_source_config_init(&config);
    config.read = read_input;
    config.reset = reset;
    config.close = close_input;
    config.userdata = &input;
    CHECK(knurl_source_new(&config, &input.source, &error) == KNURL_OK);
    memset(&closing, 0, sizeof(closing));
    closing.source = input.source;
    knurl_source_config_init(&config);
    config.read = empty;
    config.close = close_previous;
    config.userdata = &closing;
    CHECK(knurl_source_new(&config, &previous, &error) == KNURL_OK);
    CHECK(knurl_reply_source_take(reply, 200, "text/plain", previous, &error) ==
          KNURL_OK);
    fd = next_fd();
    CHECK(fd >= 3);
    CHECK(knurl_reply_source_spooled(reply, 202, "text/plain", input.source,
                                     ".", &error) == KNURL_OK);
    CHECK(closing.closed == 1 && closing.result == KNURL_STATE);
    CHECK(input.resets == 1 && input.reads > 1000 && input.remaining == 0 &&
          input.closes == 0);
    CHECK(input.reenter_reply == KNURL_STATE &&
          input.reenter_source == KNURL_STATE);
    CHECK(knurl_reply_payload_kind(reply) == KNURL_REPLY_FILE &&
          knurl_reply_status_code(reply) == 202);
    CHECK(knurl_reply_body(reply).length == 0 &&
          knurl_reply_header_count(reply) == 1);
    CHECK(fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
          info.st_size == (off_t)input.total && info.st_nlink == 0);
    CHECK((info.st_mode & 077) == 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(pread(fd, bytes, 1, 0) == 1 && bytes[0] == 'q');
    CHECK(pread(fd, bytes, 1, (off_t)input.total - 1) == 1 && bytes[0] == 'q');
    CHECK(knurl_source_destroy(input.source, &error) == KNURL_OK &&
          input.closes == 1 && input.outcome == KNURL_OK);
    CHECK(fcntl(fd, F_GETFD) >= 0);
    CHECK(knurl_reply_text(reply, 201, "text/plain", "prior", &error) ==
          KNURL_OK);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    /* The borrowed input also stays pinned while a buffered replacement
     * releases the previous payload's close callback. */
    memset(&input, 0, sizeof(input));
    input.reply = reply;
    input.total = 23;
    knurl_source_config_init(&config);
    config.read = read_input;
    config.reset = reset;
    config.close = close_input;
    config.userdata = &input;
    CHECK(knurl_source_new(&config, &input.source, &error) == KNURL_OK);
    memset(&closing, 0, sizeof(closing));
    closing.source = input.source;
    knurl_source_config_init(&config);
    config.read = empty;
    config.close = close_previous;
    config.userdata = &closing;
    CHECK(knurl_source_new(&config, &previous, &error) == KNURL_OK);
    CHECK(knurl_reply_source_take(reply, 200, "text/plain", previous, &error) ==
          KNURL_OK);
    knurl_source_materialize_config_init(&materialize);
    materialize.max_bytes = 64;
    materialize.chunk_bytes = 7;
    materialize.reset_before = materialize.reset_after = 1;
    CHECK(knurl_reply_source_materialized(reply, 200, "text/plain",
                                          input.source, &materialize,
                                          &error) == KNURL_OK);
    CHECK(closing.closed == 1 && closing.result == KNURL_STATE &&
          input.resets == 2 && input.closes == 0);
    CHECK(knurl_reply_body(reply).length == 23 &&
          knurl_source_destroy(input.source, NULL) == KNURL_OK);
    CHECK(knurl_reply_text(reply, 201, "text/plain", "prior", &error) ==
          KNURL_OK);
    for (mode = 1; mode <= 4; ++mode) {
        memset(&input, 0, sizeof(input));
        input.reply = reply;
        input.total = 20000;
        input.mode = mode;
        knurl_source_config_init(&config);
        config.read = read_input;
        config.reset = reset;
        config.close = close_input;
        config.userdata = &input;
        CHECK(knurl_source_new(&config, &input.source, &error) == KNURL_OK);
        fd = next_fd();
        CHECK(fd >= 3);
        status = knurl_reply_source_spooled(
            reply, 203, "text/plain", input.source,
            mode == 4 ? "missing-spool-directory" : ".", &error);
        CHECK(status == (mode == 1 ? KNURL_NOT_SUPPORTED : KNURL_IO));
        CHECK(next_fd() == fd && knurl_reply_status_code(reply) == 201 &&
              knurl_reply_payload_kind(reply) == KNURL_REPLY_BYTES);
        CHECK(knurl_reply_body(reply).length == 5 &&
              !memcmp(knurl_reply_body(reply).data, "prior", 5));
        if (mode == 2 || mode == 3)
            CHECK(error.native_code == 321 &&
                  !strcmp(error.message,
                          mode == 2 ? "reset-first" : "read-first"));
        if (mode == 4)
            CHECK(error.source == KNURL_ERROR_SYSTEM &&
                  error.native_code == ENOENT && input.resets == 0 &&
                  input.reads == 0);
        CHECK(input.closes == 0 &&
              knurl_source_destroy(input.source, NULL) == KNURL_OK &&
              input.closes == 1);
    }
    /* A real mid-write filesystem rejection must close the partial file. */
    memset(&input, 0, sizeof(input));
    input.reply = reply;
    input.total = 20000;
    knurl_source_config_init(&config);
    config.read = read_input;
    config.close = close_input;
    config.userdata = &input;
    input.remaining = input.total;
    CHECK(knurl_source_new(&config, &input.source, &error) == KNURL_OK);
    CHECK(getrlimit(RLIMIT_FSIZE, &saved) == 0);
    limited = saved;
    limited.rlim_cur = 8192;
    handler = signal(SIGXFSZ, SIG_IGN);
    CHECK(handler != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limited) == 0);
    fd = next_fd();
    status = knurl_reply_source_spooled(reply, 200, "text/plain", input.source,
                                        ".", &error);
    CHECK(setrlimit(RLIMIT_FSIZE, &saved) == 0 &&
          signal(SIGXFSZ, handler) != SIG_ERR);
    CHECK(status == KNURL_IO && error.source == KNURL_ERROR_SYSTEM &&
          error.native_code == EFBIG && next_fd() == fd);
    CHECK(knurl_reply_status_code(reply) == 201 &&
          knurl_source_destroy(input.source, NULL) == KNURL_OK);
    /* Synchronous JSON must not retain a stack record or its later contents. */
    record.message = bytes;
    strcpy(bytes, "before");
    CHECK(knurl_json_schema_from_map(NULL, &map, &schema, &error) == KNURL_OK);
    fd = next_fd();
    CHECK(knurl_reply_json_spooled(reply, 200, &schema, &record, ".", &error) ==
          KNURL_OK);
    memset(bytes, 'x', sizeof(bytes));
    record.message = NULL;
    CHECK(fstat(fd, &info) == 0 && info.st_size == (off_t)strlen(expected) &&
          info.st_nlink == 0);
    CHECK(pread(fd, bytes, strlen(expected), 0) == (ssize_t)strlen(expected) &&
          !memcmp(bytes, expected, strlen(expected)));
    CHECK(knurl_reply_json_spooled(reply, 201, &schema, NULL, ".", &error) ==
              KNURL_INVALID &&
          fcntl(fd, F_GETFD) >= 0);
    record.message = "disk failure";
    limited.rlim_cur = 1;
    handler = signal(SIGXFSZ, SIG_IGN);
    CHECK(handler != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limited) == 0);
    status =
        knurl_reply_json_spooled(reply, 203, &schema, &record, ".", &error);
    CHECK(setrlimit(RLIMIT_FSIZE, &saved) == 0 &&
          signal(SIGXFSZ, handler) != SIG_ERR);
    CHECK(status == KNURL_IO && error.source == KNURL_ERROR_SYSTEM &&
          error.native_code == EFBIG);
    CHECK(knurl_reply_status_code(reply) == 200 && fcntl(fd, F_GETFD) >= 0);
    CHECK(knurl_reply_destroy(reply, &error) == KNURL_OK &&
          fcntl(fd, F_GETFD) == -1);
    return 0;
}
