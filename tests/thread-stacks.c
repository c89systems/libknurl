#define _GNU_SOURCE 1
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "test thread stacks %d: %s\n", __LINE__, #t);      \
            abort();                                                           \
        }                                                                      \
    } while (0)
static size_t default_size(void)
{
    pthread_attr_t attributes;
    size_t size;
    CHECK(pthread_getattr_default_np(&attributes) == 0);
    CHECK(pthread_attr_getstacksize(&attributes, &size) == 0);
    CHECK(pthread_attr_destroy(&attributes) == 0);
    return size;
}
#if defined(KNURL_STACK_CONTROL)
int main(int argc, char **argv)
{
    struct rlimit before, after;
    size_t size = default_size();
    pid_t child;
    int status;
    CHECK(argc == 2);
    CHECK(getrlimit(RLIMIT_STACK, &before) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(default_size() == size);
    CHECK(getrlimit(RLIMIT_STACK, &after) == 0 &&
          before.rlim_cur == after.rlim_cur &&
          before.rlim_max == after.rlim_max);
    /* A direct invocation without loader-time cache control must fail closed.
     */
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(unsetenv("GLIBC_TUNABLES") == 0);
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 2);
    return 0;
}
#else
struct worker_arguments {
    size_t size, guard;
    int nested, probe_guard;
};
static void guard_fault(void *base)
{
    pid_t child;
    int status;
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        volatile unsigned char *guard = (volatile unsigned char *)base - 1;
        volatile unsigned char byte = *guard;
        (void)byte;
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
          (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}
static void *worker(void *data)
{
    struct worker_arguments *arguments = data, nested;
    pthread_attr_t attributes;
    pthread_t thread;
    size_t size, guard;
    void *base;
    volatile unsigned char readable;
    CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
    CHECK(pthread_attr_getstack(&attributes, &base, &size) == 0);
    CHECK(pthread_attr_getguardsize(&attributes, &guard) == 0);
    CHECK(pthread_attr_destroy(&attributes) == 0);
    CHECK(size == arguments->size && guard >= arguments->guard);
    readable = *(volatile unsigned char *)base;
    (void)readable;
    if (arguments->probe_guard)
        guard_fault(base);
    if (arguments->nested) {
        nested = *arguments;
        nested.nested = 0;
        CHECK(pthread_create(&thread, NULL, worker, &nested) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
    }
    return NULL;
}
int main(void)
{
    pthread_attr_t attributes;
    pthread_t thread;
    struct worker_arguments arguments;
    struct rlimit core;
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0 && default_size() == 128u * 1024u);
    core.rlim_cur = core.rlim_max = 0;
    CHECK(setrlimit(RLIMIT_CORE, &core) == 0);
    arguments.size = 1024u * 1024u;
    arguments.guard = (size_t)page * 2u;
    arguments.nested = 0;
    arguments.probe_guard = 1;
    CHECK(pthread_attr_init(&attributes) == 0);
    CHECK(pthread_attr_setstacksize(&attributes, arguments.size) == 0);
    CHECK(pthread_attr_setguardsize(&attributes, arguments.guard) == 0);
    CHECK(pthread_create(&thread, &attributes, worker, &arguments) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_attr_destroy(&attributes) == 0);
    /* Warm a larger allocation first: an enabled glibc stack cache can make
     * this following default worker larger than its requested nominal size. */
    arguments.size = 128u * 1024u;
    arguments.guard = (size_t)page;
    arguments.nested = 1;
    CHECK(pthread_create(&thread, NULL, worker, &arguments) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    /* Match the executor's explicit attributes with no explicit stack size. */
    arguments.nested = 0;
    CHECK(pthread_attr_init(&attributes) == 0);
    CHECK(pthread_create(&thread, &attributes, worker, &arguments) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_attr_destroy(&attributes) == 0);
    return 0;
}
#endif
