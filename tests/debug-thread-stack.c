#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stack_failure(const char *operation, int code)
{
    fprintf(stderr, "Debug test thread stacks: %s: %s\n", operation,
            strerror(code));
    _exit(2);
}
static int stack_cache_disabled(void)
{
    static const char key[] = "glibc.pthread.stack_cache_size=";
    const char *value = getenv("GLIBC_TUNABLES"), *end;
    size_t length;
    int disabled = 0;
    while (value != NULL && *value != '\0') {
        end = strchr(value, ':');
        length = end != NULL ? (size_t)(end - value) : strlen(value);
        if (length >= sizeof(key) - 1u && !memcmp(value, key, sizeof(key) - 1u))
            disabled = length == sizeof(key) && value[sizeof(key) - 1u] == '0';
        value = end != NULL ? end + 1 : NULL;
    }
    return disabled;
}
static void debug_thread_stacks(void) __attribute__((constructor));
static void debug_thread_stacks(void)
{
    pthread_attr_t attributes;
    struct rlimit before, after;
    size_t guard;
    long page;
    int code;
    if (!stack_cache_disabled()) {
        fprintf(stderr, "Debug tests require GLIBC_TUNABLES="
                        "glibc.pthread.stack_cache_size=0 at startup; "
                        "run through CTest or supply it explicitly\n");
        _exit(2);
    }
    if (getrlimit(RLIMIT_STACK, &before) != 0)
        stack_failure("getrlimit", errno);
    code = pthread_getattr_default_np(&attributes);
    if (code != 0)
        stack_failure("pthread_getattr_default_np", code);
    code = pthread_attr_setstacksize(&attributes, 128u * 1024u);
    if (code != 0)
        stack_failure("pthread_attr_setstacksize", code);
    code = pthread_attr_getguardsize(&attributes, &guard);
    if (code != 0)
        stack_failure("pthread_attr_getguardsize", code);
    if (guard == 0) {
        page = sysconf(_SC_PAGESIZE);
        if (page <= 0)
            stack_failure("page size", EINVAL);
        code = pthread_attr_setguardsize(&attributes, (size_t)page);
        if (code != 0)
            stack_failure("pthread_attr_setguardsize", code);
    }
    code = pthread_setattr_default_np(&attributes);
    if (code != 0)
        stack_failure("pthread_setattr_default_np", code);
    code = pthread_attr_destroy(&attributes);
    if (code != 0)
        stack_failure("pthread_attr_destroy", code);
    if (getrlimit(RLIMIT_STACK, &after) != 0)
        stack_failure("getrlimit", errno);
    if (before.rlim_cur != after.rlim_cur || before.rlim_max != after.rlim_max)
        stack_failure("main-thread stack limit changed", EINVAL);
}
