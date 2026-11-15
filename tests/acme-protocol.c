#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include "acme-ca.h"
#include "vectis-services.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "ACME protocol line %d: %s\n", __LINE__,     \
                          #test);                                              \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    struct test_ca *ca;
    knurl_server *server;
    char *root;
    char *ca_file;
    char *directory_url;
    int notes[2];
    int mode;
    int phase;
    int persisted;
    int restored;
    int restarted;
    int failure;
    pid_t acme;
    pid_t client;
    pid_t parent;
};

static struct fixture *active_fixture;

static void cleanup(void)
{
    struct fixture *fixture;
    fixture = active_fixture;
    if (fixture == NULL || fixture->parent != getpid())
        return;
    if (fixture->client > 0) {
        (void)kill(fixture->client, SIGKILL);
        (void)waitpid(fixture->client, NULL, 0);
        fixture->client = 0;
    }
    test_ca_free(fixture->ca);
    fixture->ca = NULL;
    if (fixture->root != NULL)
        (void)test_ca_remove(fixture->root);
}

static reference_status restore(const char *directory,
                                const char *const *domains, size_t domain_count,
                                void *data, reference_error *error)
{
    struct fixture *fixture = data;
    char *name;
    size_t size;
    (void)error;
    if (getpid() != fixture->parent || domain_count != 1 ||
        strcmp(domains[0], "localhost") != 0 ||
        strcmp(directory, fixture->root))
        return REFERENCE_INVALID;
    ++fixture->restored;
    size = strlen(directory) + 64;
    name = malloc(size);
    if (name == NULL)
        return REFERENCE_NOMEM;
    (void)snprintf(name, size, "%s/certificates", (const char *)directory);
    if (mkdir(name, 0700) != 0 && errno != EEXIST) {
        free(name);
        return REFERENCE_STATE;
    }
    (void)snprintf(name, size, "%s/certificates/localhost",
                   (const char *)directory);
    if (mkdir(name, 0700) != 0 && errno != EEXIST) {
        free(name);
        return REFERENCE_STATE;
    }
    free(name);
    if (!test_ca_write(directory, "account-key.pem",
                       fixture->ca->account_pem) ||
        !test_ca_write(directory, "certificates/localhost/key.pem",
                       fixture->ca->domain_pem))
        return REFERENCE_STATE;
    return REFERENCE_OK;
}

static reference_status persist(const char *directory,
                                const char *const *domains, size_t domain_count,
                                void *data, reference_error *error)
{
    struct fixture *fixture = data;
    const char note = 'P';
    (void)error;
    if (getpid() == fixture->parent || domain_count != 1 ||
        strcmp(domains[0], "localhost") != 0 ||
        strcmp(directory, fixture->root))
        return REFERENCE_INVALID;
    return write(fixture->notes[1], &note, 1) == 1 ? REFERENCE_OK
                                                   : REFERENCE_STATE;
}

static knurl_status process_start(knurl_server *server,
                                  const knurl_process_context *process,
                                  void *userdata, knurl_error *error)
{
    (void)server;
    (void)userdata;
    (void)error;
    if (process->role == KNURL_PROCESS_ACME) {
        if (setenv("SSL_CERT_FILE", "/knurl-changed-after-fork", 1) != 0 ||
            setenv("SSL_CERT_DIR", "/knurl-changed-directory-after-fork", 1) !=
                0)
            return KNURL_IO;
    }
    return KNURL_OK;
}

static void client(struct fixture *fixture, int phase)
{
    size_t i;
    int okay;
    int attempt;
    struct timespec delay;
    char note;
    fixture->client = fork();
    if (fixture->client != 0)
        return;
    (void)signal(SIGPIPE, SIG_IGN);
    delay.tv_sec = 0;
    delay.tv_nsec = 2000000;
    okay = 1;
    for (i = 0; i < 2; ++i) {
        for (attempt = 0; attempt < 100; ++attempt) {
            if (test_ca_client(fixture->ca, fixture->ca->report->ports[i],
                               phase == 1 ? 1 : 2, phase == 3 ? 200 : 503, 0))
                break;
            (void)nanosleep(&delay, NULL);
        }
        if (attempt == 100)
            okay = 0;
    }
    note = okay ? (char)('0' + phase) : 'F';
    (void)write(fixture->notes[1], &note, 1);
    _exit(okay ? 0 : 84);
}

static void notes(knurl_watch *watch, unsigned int events, void *userdata)
{
    struct fixture *fixture;
    char buffer[32];
    ssize_t count;
    size_t i;
    int status;
    pid_t waited;
    (void)watch;
    (void)events;
    fixture = userdata;
    count = read(fixture->notes[0], buffer, sizeof(buffer));
    for (i = 0; count > 0 && i < (size_t)count; ++i) {
        if (buffer[i] == 'K')
            continue;
        if (buffer[i] == 'P') {
            ++fixture->persisted;
            continue;
        }
        status = 0;
        do {
            waited = waitpid(fixture->client, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != fixture->client || !WIFEXITED(status) ||
            WEXITSTATUS(status)) {
            (void)fprintf(stderr,
                          "ACME protocol client wait: expected %ld, got %ld, "
                          "status %d, errno %d\n",
                          (long)fixture->client, (long)waited, status,
                          waited < 0 ? errno : 0);
            fixture->failure = 1;
        }
        fixture->client = 0;
        if (buffer[i] == '1') {
            fixture->phase = 2;
            if (kill(fixture->acme, SIGKILL) != 0)
                fixture->failure = 1;
        } else if (buffer[i] == '2') {
            fixture->phase = 4;
            if (knurl_server_set_serving(fixture->server, 1, NULL) != KNURL_OK)
                fixture->failure = 1;
            client(fixture, 3);
        } else {
            if (buffer[i] != '3')
                fixture->failure = 1;
            fixture->phase = 5;
            (void)knurl_server_request_stop(fixture->server, NULL);
        }
    }
}

static void stop(knurl_timer *timer, void *userdata)
{
    struct fixture *fixture;
    (void)timer;
    fixture = userdata;
    if (fixture->mode != 3 && fixture->mode != 4)
        fixture->failure = 1;
    (void)knurl_server_request_stop(fixture->server, NULL);
}

static void progress(knurl_timer *timer, void *userdata)
{
    struct fixture *fixture;
    knurl_tls_identity_state current;
    (void)timer;
    fixture = userdata;
    if (__atomic_load_n(&fixture->ca->report->failed, __ATOMIC_ACQUIRE))
        fixture->failure = 1;
    if (fixture->failure) {
        (void)knurl_server_request_stop(fixture->server, NULL);
        return;
    }
    if (fixture->mode == 3 || fixture->mode == 4)
        return;
    if (knurl_server_tls_identity_state(fixture->server, 0, &current, NULL) !=
        KNURL_OK)
        fixture->failure = 1;
    if (current != KNURL_TLS_USABLE || fixture->client > 0)
        return;
    if (fixture->phase == 0 && fixture->persisted >= 1) {
        fixture->phase = 1;
        client(fixture, 1);
    } else if (fixture->phase == 2 && fixture->persisted >= 2) {
        fixture->phase = 3;
        client(fixture, 2);
    }
}

static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *userdata,
                          knurl_error *error)
{
    struct fixture *fixture;
    knurl_bound_listener listener;
    knurl_watch *watch;
    knurl_timer *timer;
    size_t i;
    (void)process;
    fixture = userdata;
    for (i = 0; i < 2; ++i) {
        if (knurl_server_listener_at(server, i, &listener, error) != KNURL_OK ||
            !listener.tls)
            return KNURL_INVALID;
        fixture->ca->report->ports[i] = listener.port;
    }
    __atomic_store_n(&fixture->ca->report->ports_ready, 1, __ATOMIC_RELEASE);
    if (knurl_loop_watch(knurl_server_loop(server), fixture->notes[0],
                         KNURL_READABLE, notes, NULL, fixture, &watch,
                         error) != KNURL_OK ||
        knurl_loop_timer(knurl_server_loop(server), 0, 10, progress, NULL,
                         fixture, &timer, error) != KNURL_OK ||
        knurl_loop_timer(
            knurl_server_loop(server),
            fixture->mode == 3 ? 250 : (fixture->mode == 4 ? 1200 : 6000), 0,
            stop, NULL, fixture, &timer, error) != KNURL_OK)
        return KNURL_INVALID;
    return KNURL_OK;
}

static void worker_event(knurl_server *server, const knurl_worker_event *event,
                         void *userdata)
{
    struct fixture *fixture;
    (void)server;
    fixture = userdata;
    if (event->process.role != KNURL_PROCESS_ACME)
        return;
    if (event->kind == KNURL_WORKER_STARTED ||
        event->kind == KNURL_WORKER_RESTARTED)
        fixture->acme = (pid_t)event->process.process_id;
    if (event->kind == KNURL_WORKER_RESTARTED)
        ++fixture->restarted;
}

static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *userdata, knurl_error *error)
{
    (void)request;
    (void)userdata;
    return knurl_reply_text(reply, 200, "text/plain", "ACME issued", error);
}

int main(int argc, char **argv)
{
    struct fixture *fixture;
    knurl_server_config config;
    knurl_server_config secure;
    struct reference_tls_config baseline;
    struct reference_tls_binding binding;
    reference_error host_error;
    knurl_listener_config listeners[2];
    knurl_tls_identity identity;
    knurl_route_config route;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    const char *domain = "localhost";
    char *directory;
    size_t i;
    fixture = calloc(1, sizeof(*fixture));
    CHECK(fixture != NULL);
    fixture->parent = getpid();
    active_fixture = fixture;
    CHECK(atexit(cleanup) == 0);
    fixture->mode =
        argc < 2
            ? 0
            : (!strcmp(argv[1], "environment")
                   ? 1
                   : (!strcmp(argv[1], "directory")
                          ? 2
                          : (!strcmp(argv[1], "limit")
                                 ? 3
                                 : (!strcmp(argv[1], "timeout") ? 4 : 5))));
    directory = malloc(64);
    CHECK(directory != NULL);
    (void)strcpy(directory, "knurl-acme-protocol-XXXXXX");
    CHECK(mkdtemp(directory) != NULL);
    fixture->root = realpath(directory, NULL);
    CHECK(fixture->root != NULL);
    fixture->ca = test_ca_new(fixture->root, fixture->mode);
    CHECK(fixture->ca != NULL);
    fixture->ca_file = malloc(strlen(fixture->root) + 16);
    fixture->directory_url = malloc(strlen(fixture->ca->base) + 16);
    CHECK(fixture->ca_file != NULL && fixture->directory_url != NULL);
    (void)sprintf(fixture->ca_file, "%s/ca.pem", fixture->root);
    (void)sprintf(fixture->directory_url, "%s/directory", fixture->ca->base);
    CHECK(unsetenv("SSL_CERT_DIR") == 0);
    CHECK(setenv("SSL_CERT_FILE",
                 fixture->mode == 1 ? fixture->ca_file
                                    : "/knurl-invalid-environment-ca",
                 1) == 0);
    if (fixture->mode == 2) {
        CHECK(unsetenv("SSL_CERT_FILE") == 0);
        CHECK(setenv("SSL_CERT_DIR", "/knurl-invalid-environment-ca-directory",
                     1) == 0);
    }
    CHECK(setenv("no_proxy", "127.0.0.1,localhost", 1) == 0);
    CHECK(test_ca_start(fixture->ca));
    CHECK(pipe(fixture->notes) == 0 &&
          fcntl(fixture->notes[0], F_SETFL, O_NONBLOCK) == 0);
    knurl_server_config_init(&config);
    for (i = 0; i < 2; ++i) {
        knurl_listener_config_init(&listeners[i]);
        listeners[i].bind = "127.0.0.1";
        listeners[i].kind = KNURL_LISTENER_HTTPS;
    }
    knurl_tls_identity_init(&identity);
    identity.mode = KNURL_TLS_ACME;
    identity.domains = &domain;
    identity.domain_count = 1;
    config.listeners = listeners;
    config.listener_count = 2;
    config.workers.count = 2;
    config.workers.affinity_disabled = 1;
    config.workers.shutdown_timeout_ms = 500;
    config.http.backend_quiet = 1;
    config.http.backend_curl_timeout_seconds = 1;
    if (fixture->mode == 3)
        config.http.backend_curl_recv_max_bytes.low = 64;
    memset(&baseline, 0, sizeof(baseline));
    baseline.mode = KNURL_TLS_ACME;
    baseline.bind = "127.0.0.1";
    baseline.domains = &domain;
    baseline.domain_count = 1;
    baseline.acme_email = "fixture@example.test";
    baseline.acme_directory_url = fixture->directory_url;
    baseline.acme_state_dir = fixture->root;
    baseline.state.hydrate = restore;
    baseline.state.persist = persist;
    baseline.state.userdata = fixture;
    knurl_server_config_init(&secure);
    CHECK(reference_tls_translate(&baseline, &secure, &binding, &host_error) ==
          REFERENCE_OK);
    identity = binding.identity;
    config.tls = secure.tls;
    config.tls.identities = &identity;
    config.tls.identity_count = 1;
    config.tls.acme.email = "fixture@example.test";
    config.tls.acme.directory_url = fixture->directory_url;
    config.tls.acme.runtime_directory = fixture->root;
    if (fixture->mode != 1 && fixture->mode != 2)
        config.tls.acme.ca_file = fixture->ca_file;
    if (fixture->mode == 2)
        config.tls.acme.ca_directory = fixture->root;
    config.callbacks.on_process_start = process_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_event = worker_event;
    config.callbacks.userdata = fixture;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.start_paused = 1;
    CHECK(knurl_server_new(&config, &fixture->server, &error) == KNURL_OK);
    CHECK(knurl_server_validate(fixture->server, &error) == KNURL_OK);
    CHECK(fixture->restored == 0 && fixture->ca->report->directories == 0);
    knurl_route_config_init(&route);
    route.path = "/";
    route.handler = handler;
    CHECK(knurl_router_route(knurl_server_router(fixture->server), &route, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(fixture->server, &result, &error);
    if (status != KNURL_OK || fixture->failure)
        (void)fprintf(
            stderr,
            "ACME protocol mode %d status %d %s: %s, phase %d, directory %d "
            "resolve %d create %d orders %d challenges %d csr %d issued %d "
            "persisted %d restarts %d\n",
            fixture->mode, status, error.operation, error.message,
            fixture->phase, fixture->ca->report->directories,
            fixture->ca->report->resolves, fixture->ca->report->creates,
            fixture->ca->report->orders, fixture->ca->report->challenges,
            fixture->ca->report->finalized, fixture->ca->report->issued,
            fixture->persisted, fixture->restarted);
    if (fixture->client > 0) {
        (void)kill(fixture->client, SIGKILL);
        (void)waitpid(fixture->client, NULL, 0);
    }
    CHECK(status == KNURL_OK && !fixture->failure && fixture->restored == 1);
    CHECK(!result.escalated && result.reason == KNURL_RUN_REQUESTED_STOP);
    if (fixture->mode == 3 || fixture->mode == 4) {
        CHECK(fixture->ca->report->directories == 1 &&
              fixture->ca->report->orders == 0 && fixture->persisted == 0 &&
              !result.has_failure);
    } else {
        CHECK(fixture->phase == 5 && fixture->persisted == 2 &&
              fixture->restarted == 1 && result.has_failure &&
              result.last_failure.process.role == KNURL_PROCESS_ACME &&
              result.recovered_failures.high == 0 &&
              result.recovered_failures.low == 1);
        CHECK(fixture->ca->report->resolves == (fixture->mode == 5 ? 3 : 2) &&
              fixture->ca->report->creates == 1 &&
              fixture->ca->report->orders == 2 &&
              fixture->ca->report->challenges == 2 &&
              fixture->ca->report->finalized == 2 &&
              fixture->ca->report->issued == 2);
    }
    CHECK(!fixture->ca->report->failed);
    CHECK(knurl_server_destroy(fixture->server, &error) == KNURL_OK);
    test_ca_free(fixture->ca);
    fixture->ca = NULL;
    CHECK(test_ca_remove(directory));
    (void)close(fixture->notes[0]);
    (void)close(fixture->notes[1]);
    free(directory);
    free(fixture->root);
    free(fixture->ca_file);
    free(fixture->directory_url);
    active_fixture = NULL;
    free(fixture);
    return 0;
}
