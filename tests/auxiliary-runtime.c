#include "library.h"
#include "http.h"
#include "acme.h"

#include <openssl/pem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int notes[2];
static int phase;
static int turned;
static int failure;
static int cleaning;
static unsigned int started;
static unsigned int ended;
static int mode;
static int deaths;
static int restarts;
static jmp_buf unwind;

static unsigned int role(const struct kore_worker *child)
{
    return child->id == KORE_WORKER_KEYMGR
               ? 1U
               : (child->id == KORE_WORKER_ACME ? 2U : 4U);
}

static void note(int stage)
{
    unsigned char byte;
    byte = (unsigned char)(role(worker) | (unsigned int)(stage << 3));
    if (write(notes[1], &byte, 1) != 1)
        _exit(81);
}

static void fatal_hook(void *userdata, const char *message)
{
    (void)userdata;
    (void)fprintf(stderr, "auxiliary fatal: %s\n", message);
    if (worker != NULL || cleaning)
        _exit(82);
    failure = 1;
    longjmp(unwind, 1);
}

static int process_start(void *userdata, struct kore_worker *child)
{
    (void)userdata;
    (void)child;
    phase = 1;
    turned = 0;
    note(0);
    return 1;
}

static void stop(void *userdata, u_int64_t now)
{
    (void)userdata;
    (void)now;
    kore_shutdown();
}

static void tick(void *userdata, u_int64_t now)
{
    (void)userdata;
    (void)now;
}

static int worker_start(void *userdata)
{
    (void)userdata;
    if (phase != 1)
        _exit(83);
    if (worker->id >= KORE_WORKER_KEYMGR && !worker->no_accept)
        _exit(87);
    phase = 2;
    note(1);
    (void)kore_timer_add(tick, 1, NULL, KORE_TIMER_ONESHOT);
    if (worker->id == KORE_WORKER_KEYMGR && (mode == 0 || worker->restarted))
        (void)kore_timer_add(stop, 25, NULL, KORE_TIMER_ONESHOT);
    return 1;
}

static u_int64_t loop_wait(void *userdata, u_int64_t maximum)
{
    (void)userdata;
    return maximum > 50 ? 50 : maximum;
}

static void loop_turn(void *userdata)
{
    (void)userdata;
    if (worker != NULL && !turned) {
        turned = 1;
        note(2);
    }
}

static void stopping(void *userdata)
{
    (void)userdata;
    if (phase != 2)
        _exit(84);
    phase = 3;
    note(3);
}

static void cleanup(void *userdata)
{
    (void)userdata;
    if (phase != 3)
        _exit(85);
    phase = 4;
    note(4);
}

static int worker_exit(void *userdata)
{
    (void)userdata;
    if (phase != 4 || !LIST_EMPTY(&kore_servers) || !TAILQ_EMPTY(&connections))
        return 86;
    note(5);
    __atomic_store_n(&worker->library_clean, 1, __ATOMIC_RELEASE);
    return 0;
}

static void event(void *userdata, const struct kore_worker *child, int kind,
                  int status, int expected, int abandoned)
{
    (void)userdata;
    if (kind == KORE_LIBRARY_WORKER_STARTED) {
        started |= role(child);
        if (mode != 0 && child->id == KORE_WORKER_KEYMGR &&
            kill(child->pid, SIGKILL) != 0)
            failure = 1;
    } else if (kind == KORE_LIBRARY_WORKER_RESTARTED) {
        ++restarts;
        if (mode != 1 || child->id != KORE_WORKER_KEYMGR)
            failure = 1;
    } else if (kind == KORE_LIBRARY_WORKER_EXITED) {
        if (!expected) {
            ++deaths;
            if (mode == 0 || child->id != KORE_WORKER_KEYMGR || !abandoned ||
                !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
                failure = 1;
        } else if (abandoned || !WIFEXITED(status) || WEXITSTATUS(status))
            failure = 1;
        ended |= role(child);
    } else if (kind != KORE_LIBRARY_WORKER_SPAWNING)
        failure = 1;
}

static int input(const char *path)
{
    EVP_PKEY *key;
    FILE *file;
    int okay;
    key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    if (key == NULL)
        return 0;
    file = fopen(path, "w");
    okay = file != NULL &&
           PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL);
    if (file != NULL && fclose(file) != 0)
        okay = 0;
    EVP_PKEY_free(key);
    return okay;
}

int main(int argc, char **argv)
{
    struct kore_library_hooks hooks = {0};
    struct kore_server *server;
    struct kore_domain *domain;
    unsigned int stages[6] = {0};
    unsigned char bytes[32];
    u_int64_t deadline;
    ssize_t count;
    size_t i;
    char *directory;
    char *path;
    char *absolute;
    int okay;
    mode = argc > 1 ? (strcmp(argv[1], "restart") == 0 ? 1 : 2) : 0;
    directory = malloc(64);
    path = malloc(4096);
    absolute = malloc(4096);
    if (directory == NULL || path == NULL || absolute == NULL)
        return 1;
    (void)strcpy(directory, "rotmos-auxiliary-XXXXXX");
    if (mkdtemp(directory) == NULL || getcwd(absolute, 4096) == NULL)
        return 2;
    (void)snprintf(path, 4096, "%s/account-key.pem", directory);
    if (!input(path) || pipe(notes) != 0 ||
        fcntl(notes[0], F_SETFL, O_NONBLOCK) != 0)
        return 3;
    (void)snprintf(path, 4096, "%s/%s", absolute, directory);
    hooks.fatal = fatal_hook;
    hooks.process_start = process_start;
    hooks.worker_start = worker_start;
    hooks.worker_stopping = stopping;
    hooks.worker_cleanup = cleanup;
    hooks.worker_exit = worker_exit;
    hooks.worker_event = event;
    hooks.loop_wait = loop_wait;
    hooks.loop_turn = loop_turn;
    if (setjmp(unwind) == 0) {
        kore_library_init(&hooks);
        kore_quiet = 1;
        worker_count = 1;
        worker_set_affinity = 0;
        worker_policy = mode == 2 ? KORE_WORKER_POLICY_TERMINATE
                                  : KORE_WORKER_POLICY_RESTART;
        worker_shutdown_timeout_ms = 1000;
        kore_library_startup_timeout_ms = 2000;
        server = kore_server_create("auxiliary fixture");
        server->tls = 0;
        if (!kore_server_bind(server, "127.0.0.1", "0", NULL))
            fatal("cannot bind auxiliary fixture");
        domain = kore_domain_new("*");
        if (!kore_domain_attach(domain, server))
            fatal("cannot attach auxiliary fixture");
        keymgr_privsep.root = path;
        acme_privsep.root = path;
        acme_domains = 1;
        kore_free(acme_provider);
        acme_provider = kore_strdup("http://127.0.0.1:1/directory");
        kore_free(acme_email);
        acme_email = kore_strdup("fixture@example.test");
        if (!kore_library_parent_start())
            failure = 1;
        deadline = kore_time_ms() + 2000;
        while (!failure && kore_quit == KORE_QUIT_NONE &&
               kore_time_ms() < deadline)
            kore_library_parent_step(100);
        if (kore_quit != (mode == 2 ? KORE_QUIT_FATAL : KORE_QUIT_NORMAL))
            failure = 1;
    }
    cleaning = 1;
    kore_library_cleanup();
    cleaning = 0;
    while ((count = read(notes[0], bytes, sizeof(bytes))) > 0) {
        for (i = 0; i < (size_t)count; ++i) {
            if ((bytes[i] >> 3) >= 6)
                failure = 1;
            else
                stages[bytes[i] >> 3] |= bytes[i] & 7U;
        }
    }
    okay = started == 7 && ended == 7 && !failure && deaths == (mode != 0) &&
           restarts == (mode == 1);
    for (i = 0; i < 6; ++i) {
        if ((stages[i] & (mode == 2 && i >= 2 ? 6U : 7U)) !=
            (mode == 2 && i >= 2 ? 6U : 7U)) {
            (void)fprintf(stderr, "auxiliary stage %lu: mask %u\n",
                          (unsigned long)i, stages[i]);
            okay = 0;
        }
    }
    (void)close(notes[0]);
    (void)close(notes[1]);
    (void)snprintf(path, 4096, "%s/account-key.pem", directory);
    if (unlink(path) != 0)
        okay = 0;
    (void)snprintf(path, 4096, "%s/certificates", directory);
    if (rmdir(path) != 0 || rmdir(directory) != 0)
        okay = 0;
    free(directory);
    free(path);
    free(absolute);
    return okay ? 0 : 4;
}
