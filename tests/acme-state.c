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
static rotmos_status state(rotmos_server *server,
                           const rotmos_process_context *process,
                           rotmos_acme_event event, rotmos_bytes domain,
                           rotmos_bytes directory, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    struct timespec pause;
    (void)server;
    if (strcmp(directory.data, f->root) ||
        process->process_id != (long)getpid())
        f->failed = 1;
    if (event == ROTMOS_ACME_ERROR) {
        ++f->errors;
        if (error->status == ROTMOS_OK)
            f->failed = 1;
        rotmos_error_clear(error);
        error->status = ROTMOS_NOMEM;
        strcpy(error->message, "observer must not replace cause");
        return ROTMOS_NOMEM;
    }
    ++f->calls;
    if ((event == ROTMOS_ACME_RESTORE && domain.length != 0) ||
        (event == ROTMOS_ACME_PERSIST && strcmp(domain.data, "localhost")))
        f->failed = 1;
    if (f->mode == 1) {
        error->status = ROTMOS_CONFLICT;
        strcpy(error->message, "host persistence marker");
        return ROTMOS_CONFLICT;
    }
    if (f->mode == 2)
        return ROTMOS_AGAIN;
    if (f->mode == 3)
        return ROTMOS_EOF;
    if (f->mode == 4) {
        pause.tv_sec = 0;
        pause.tv_nsec = 5000000;
        (void)nanosleep(&pause, NULL);
    }
    return ROTMOS_OK;
}
int main(void)
{
    char root[] = "acme-state-XXXXXX";
    char *absolute = NULL;
    char path[256];
    rotmos_tls_config config;
    rotmos_process_context process;
    rotmos_error error;
    struct fixture f;
    FILE *file;
    int mode;
    CHECK(mkdtemp(root) != NULL);
    memset(&f, 0, sizeof(f));
    memset(&process, 0, sizeof(process));
    rotmos_tls_config_init(&config);
    config.acme.runtime_directory = root;
    config.acme.state = state;
    config.acme.userdata = &f;
    CHECK(rotmos_acme_runtime_directory(&config, &absolute, &error) ==
          ROTMOS_OK);
    f.root = absolute;
    CHECK(f.calls == 0 && f.errors == 0);
    process.process_id = (long)getpid();
    process.role = ROTMOS_PROCESS_PARENT;
    CHECK(rotmos_acme_state_call(&config, NULL, &process, ROTMOS_ACME_RESTORE,
                                 NULL, absolute, &error) == ROTMOS_OK);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    file = fopen(path, "wb");
    CHECK(file != NULL && fwrite("host state", 1, 10, file) == 10 &&
          !fclose(file));
    process.role = ROTMOS_PROCESS_KEY_MANAGER;
    CHECK(rotmos_acme_state_call(&config, NULL, &process, ROTMOS_ACME_PERSIST,
                                 "localhost", absolute, &error) == ROTMOS_OK);
    for (mode = 1; mode <= 4; ++mode) {
        f.mode = mode;
        config.acme.persist_timeout_ms = mode == 4 ? 1 : 0;
        CHECK(rotmos_acme_state_call(&config, NULL, &process,
                                     ROTMOS_ACME_PERSIST, "localhost", absolute,
                                     &error) == (mode == 1   ? ROTMOS_CONFLICT
                                                 : mode == 4 ? ROTMOS_TIMEOUT
                                                             : ROTMOS_INVALID));
        if (mode == 1)
            CHECK(!strcmp(error.message, "host persistence marker"));
    }
    CHECK(f.calls == 6 && f.errors == 4 && !f.failed);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    memset(path, 0, sizeof(path));
    CHECK(fread(path, 1, sizeof(path), file) == 10 && !fclose(file));
    CHECK(!strcmp(path, "host state"));
    snprintf(path, sizeof(path), "%s/.rotmos", root);
    CHECK(access(path, F_OK) != 0);
    free(absolute);
    CHECK(chmod(root, 0755) == 0);
    CHECK(rotmos_acme_runtime_directory(&config, &absolute, &error) ==
          ROTMOS_INVALID);
    CHECK(absolute == NULL && chmod(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    CHECK(!unlink(path) && !rmdir(root));
    CHECK(rotmos_acme_domain_valid("example.test") &&
          !rotmos_acme_domain_valid("../example") &&
          !rotmos_acme_domain_valid("example..test"));
    return 0;
}
