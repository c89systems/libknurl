#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "ACME state %d: %s\n", __LINE__, #t);              \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    const char *root;
    int calls, errors, mode, failed;
};
static knurl_status state(knurl_server *server,
                          const knurl_process_context *process,
                          knurl_acme_event event, knurl_bytes domain,
                          knurl_bytes directory, void *data, knurl_error *error)
{
    struct fixture *f = data;
    struct timespec pause;
    (void)server;
    if (strcmp(directory.data, f->root) ||
        process->process_id != (long)getpid())
        f->failed = 1;
    if (event == KNURL_ACME_ERROR) {
        ++f->errors;
        if (error->status == KNURL_OK)
            f->failed = 1;
        knurl_error_clear(error);
        error->status = KNURL_NOMEM;
        strcpy(error->message, "observer must not replace cause");
        return KNURL_NOMEM;
    }
    ++f->calls;
    if ((event == KNURL_ACME_RESTORE && domain.length != 0) ||
        (event == KNURL_ACME_PERSIST && strcmp(domain.data, "localhost")))
        f->failed = 1;
    if (f->mode == 1) {
        error->status = KNURL_CONFLICT;
        strcpy(error->message, "host persistence marker");
        return KNURL_CONFLICT;
    }
    if (f->mode == 2)
        return KNURL_AGAIN;
    if (f->mode == 3)
        return KNURL_EOF;
    if (f->mode == 4) {
        pause.tv_sec = 0;
        pause.tv_nsec = 5000000;
        (void)nanosleep(&pause, NULL);
    }
    return KNURL_OK;
}
int main(void)
{
    char root[] = "acme-state-XXXXXX";
    char *absolute = NULL;
    char path[256];
    knurl_tls_config config;
    knurl_process_context process;
    knurl_error error;
    struct fixture f;
    FILE *file;
    int mode;
    CHECK(mkdtemp(root) != NULL);
    memset(&f, 0, sizeof(f));
    memset(&process, 0, sizeof(process));
    knurl_tls_config_init(&config);
    config.acme.runtime_directory = root;
    config.acme.state = state;
    config.acme.userdata = &f;
    CHECK(knurl_acme_runtime_directory(&config, &absolute, &error) == KNURL_OK);
    f.root = absolute;
    CHECK(f.calls == 0 && f.errors == 0);
    process.process_id = (long)getpid();
    process.role = KNURL_PROCESS_PARENT;
    CHECK(knurl_acme_state_call(&config, NULL, &process, KNURL_ACME_RESTORE,
                                NULL, absolute, &error) == KNURL_OK);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    file = fopen(path, "wb");
    CHECK(file != NULL && fwrite("host state", 1, 10, file) == 10 &&
          !fclose(file));
    process.role = KNURL_PROCESS_KEY_MANAGER;
    CHECK(knurl_acme_state_call(&config, NULL, &process, KNURL_ACME_PERSIST,
                                "localhost", absolute, &error) == KNURL_OK);
    for (mode = 1; mode <= 4; ++mode) {
        f.mode = mode;
        config.acme.persist_timeout_ms = mode == 4 ? 1 : 0;
        CHECK(knurl_acme_state_call(&config, NULL, &process, KNURL_ACME_PERSIST,
                                    "localhost", absolute,
                                    &error) == (mode == 1   ? KNURL_CONFLICT
                                                : mode == 4 ? KNURL_TIMEOUT
                                                            : KNURL_INVALID));
        if (mode == 1)
            CHECK(!strcmp(error.message, "host persistence marker"));
    }
    CHECK(f.calls == 6 && f.errors == 4 && !f.failed);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    memset(path, 0, sizeof(path));
    CHECK(fread(path, 1, sizeof(path), file) == 10 && !fclose(file));
    CHECK(!strcmp(path, "host state"));
    snprintf(path, sizeof(path), "%s/.knurl", root);
    CHECK(access(path, F_OK) != 0);
    free(absolute);
    CHECK(chmod(root, 0755) == 0);
    CHECK(knurl_acme_runtime_directory(&config, &absolute, &error) ==
          KNURL_INVALID);
    CHECK(absolute == NULL && chmod(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    CHECK(!unlink(path) && !rmdir(root));
    CHECK(knurl_acme_domain_valid("example.test") &&
          !knurl_acme_domain_valid("../example") &&
          !knurl_acme_domain_valid("example..test"));
    return 0;
}
