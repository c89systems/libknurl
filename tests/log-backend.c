#define _POSIX_C_SOURCE 200809L
#include "../src/kore_internal.h"
#include <rotmos/pslog.h>
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
static rotmos_status started(rotmos_server *server,
                             const rotmos_process_context *process, void *data,
                             rotmos_error *error)
{
    char *large;
    unsigned char invalid[128];
    (void)server;
    (void)process;
    (void)data;
    (void)error;
    large = malloc(10001);
    if (large == NULL)
        return ROTMOS_NOMEM;
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
    return ROTMOS_OK;
}
static void stop(rotmos_timer *timer, void *data)
{
    (void)timer;
    rotmos_server_request_stop(data, NULL);
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    rotmos_timer *timer;
    (void)process;
    (void)data;
    return rotmos_loop_timer(rotmos_server_loop(server), 30, 0, stop, NULL,
                             server, &timer, error);
}
int main(void)
{
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    pslog_config logging;
    pslog_logger *root;
    rotmos_status status;
    char directory[] = "rotmos-log-backend-XXXXXX";
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
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.http.access_log_path = access_path;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_parent_ready = ready;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_set_pslog(server, root, NULL, &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
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
