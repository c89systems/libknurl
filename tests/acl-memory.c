#include <knurl/knurl.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
static void *slots[256];
static size_t live, attempts, fail_at;
static size_t find(void *pointer)
{
    size_t i;
    if (pointer != NULL)
        for (i = 0; i < 256; ++i)
            if (slots[i] == pointer)
                return i;
    return 256;
}
static int fail(void) { return ++attempts == fail_at; }
static void *remember(void *pointer)
{
    size_t i;
    if (pointer == NULL)
        return NULL;
    for (i = 0; i < 256; ++i)
        if (slots[i] == NULL) {
            slots[i] = pointer;
            ++live;
            return pointer;
        }
    abort();
    return NULL;
}
void *__wrap_malloc(size_t size)
{
    return fail() ? NULL : remember(__real_malloc(size));
}
void *__wrap_calloc(size_t count, size_t size)
{
    return fail() ? NULL : remember(__real_calloc(count, size));
}
void *__wrap_realloc(void *pointer, size_t size)
{
    size_t index = find(pointer);
    void *result;
    if (fail())
        return NULL;
    result = __real_realloc(pointer, size);
    if ((result != NULL || size == 0) && index < 256) {
        slots[index] = NULL;
        --live;
    }
    return remember(result);
}
void __wrap_free(void *pointer)
{
    size_t index = find(pointer);
    if (index < 256) {
        slots[index] = NULL;
        --live;
    }
    __real_free(pointer);
}
static knurl_status resolve(const char *name, knurl_mmdb_address *addresses,
                            size_t capacity, size_t *count, void *data,
                            knurl_error *error)
{
    (void)name;
    (void)data;
    assert(capacity == 64);
    *count = 1;
    return knurl_mmdb_address_parse("1.2.3.4", addresses, error);
}
static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    (void)request;
    (void)reply;
    (void)data;
    (void)error;
    return KNURL_STATE;
}
static void exercise(const char *path)
{
    knurl_acl_config config;
    knurl_acl *acl = NULL;
    knurl_router *router = NULL;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_route_config route;
    knurl_error error;
    const char *names[] = {"one.test", "ONE.TEST", "192.0.2.0/24"};
    const char *codes[] = {"US"};
    knurl_status status;
    knurl_acl_config_init(&config);
    config.allow.addresses = names;
    config.allow.address_count = 3;
    config.allow.countries = codes;
    config.allow.country_count = 1;
    config.deny = config.allow;
    config.country_database = path;
    config.resolve = resolve;
    status = knurl_acl_new(&config, &acl, &error);
    if (status != KNURL_OK) {
        assert(status == KNURL_NOMEM && acl == NULL);
        return;
    }
    status = acl->refresh(acl, &error);
    assert(status == KNURL_OK || status == KNURL_NOMEM);
    if (status == KNURL_OK && knurl_router_new(&router, &error) == KNURL_OK) {
        knurl_route_group_config_init(&group_config);
        group_config.acl = acl;
        if (knurl_router_group(router, &group_config, &group, &error) ==
            KNURL_OK) {
            knurl_route_config_init(&route);
            route.path = "/";
            route.acl = acl;
            route.handler = handler;
            (void)knurl_route_group_route(group, &route, NULL, &error);
        }
    }
    assert(acl->close(acl, &error) == KNURL_OK);
    assert(knurl_router_destroy(router, &error) == KNURL_OK);
}
int main(int argc, char **argv)
{
    size_t calls, i;
    assert(argc == 2);
    exercise(argv[1]);
    assert(live == 0);
    calls = attempts;
    for (i = 1; i <= calls; ++i) {
        attempts = 0;
        fail_at = i;
        exercise(argv[1]);
        assert(live == 0);
    }
    return 0;
}
