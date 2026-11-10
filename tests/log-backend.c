#define _POSIX_C_SOURCE 200809L
#include "../src/kore_internal.h"
#include <knurl/pslog.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "log backend %d: %s\n", __LINE__, #t);             \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static knurl_status started(knurl_server *server,
                            const knurl_process_context *process, void *data,
                            knurl_error *error)
{
    char *large;
    unsigned char invalid[128];
    (void)server;
    (void)process;
    (void)data;
    (void)error;
    large = malloc(10001);
    if (large == NULL)
        return KNURL_NOMEM;
    memset(large, 'x', 10000);
    large[10000] = 0;
    kore_log(LOG_INFO, "%s", large);
    free(large);
    memset(invalid, 0, sizeof(invalid));
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid, 1);
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid,
                  sizeof(invalid));
    memset(invalid, 255, sizeof(invalid));
    kore_msg_send(KORE_MSG_PARENT, KORE_MSG_WORKER_LOG, invalid,
                  sizeof(invalid));
    /* An unfinished access record must not stall the parent drain or shutdown.
     */
    worker->lb.lock = 1;
    worker->lb.offset = 1;
    return KNURL_OK;
}
static void stop(knurl_timer *timer, void *data)
{
    (void)timer;
    knurl_server_request_stop(data, NULL);
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    knurl_timer *timer;
    (void)process;
    (void)data;
    return knurl_loop_timer(knurl_server_loop(server), 30, 0, stop, NULL,
                            server, &timer, error);
}
int main(void)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    pslog_config logging;
    pslog_logger *root;
    knurl_status status;
    char directory[] = "knurl-log-backend-XXXXXX";
    char path[96], access_path[96], *line = NULL;
    size_t capacity = 0, xs;
    FILE *file;
    int bounded = 0, short_record = 0, invalid_length = 0, oversize = 0;
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/root.log", directory);
    snprintf(access_path, sizeof(access_path), "%s/access.log", directory);
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.timestamps = 0;
    CHECK(pslog_output_init_file(&logging.output, path, "a") == 0);
    root = pslog_new(&logging);
    CHECK(root != NULL);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.http.access_log_path = access_path;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_parent_ready = ready;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_set_pslog(server, root, NULL, &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    root->destroy(root);
    file = fopen(path, "r");
    CHECK(file != NULL);
    while (getline(&line, &capacity, file) >= 0) {
        char *message = strstr(line, "\"msg\":\"xxx");
        if (message != NULL) {
            message += 7;
            for (xs = 0; message[xs] == 'x'; ++xs) {
            }
            CHECK(xs == 8192 && message[xs] == '"');
            ++bounded;
        }
        if (strstr(line, "too short worker log received"))
            ++short_record;
        if (strstr(line, "invalid worker log length"))
            ++invalid_length;
        if (strstr(line, "oversize worker log received"))
            ++oversize;
    }
    free(line);
    fclose(file);
    CHECK(bounded == 1 && short_record == 1 && invalid_length == 1 &&
          oversize == 1);
    CHECK(unlink(path) == 0 && unlink(access_path) == 0 &&
          rmdir(directory) == 0);
    return 0;
}
