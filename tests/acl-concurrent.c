#define _POSIX_C_SOURCE 200809L
#include <knurl/acl.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <time.h>
struct state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int version, block, entered, released;
    knurl_acl *acl;
};
static knurl_status resolve(const char *name, knurl_mmdb_address *addresses,
                            size_t capacity, size_t *count, void *userdata,
                            knurl_error *error)
{
    struct state *state = userdata;
    int version;
    assert(capacity == 64);
    assert(pthread_mutex_lock(&state->mutex) == 0);
    version = state->version;
    if (state->block && strcmp(name, "two.test") == 0) {
        state->entered = 1;
        assert(pthread_cond_signal(&state->condition) == 0);
        while (!state->released)
            assert(pthread_cond_wait(&state->condition, &state->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&state->mutex) == 0);
    *count = 1;
    return knurl_mmdb_address_parse(version ? "2.2.2.2" : "1.1.1.1", addresses,
                                    error);
}
static void *refresh(void *userdata)
{
    struct state *state = userdata;
    knurl_error error;
    assert(knurl_acl_refresh(state->acl, &error) == KNURL_OK);
    return NULL;
}
static void check(knurl_acl *acl, const char *text, int expected)
{
    knurl_mmdb_address address;
    knurl_error error;
    int allowed;
    assert(knurl_mmdb_address_parse(text, &address, &error) == KNURL_OK);
    assert(acl->check(acl, &address, NULL, &allowed, &error) == KNURL_OK);
    assert(allowed == expected);
}
int main(void)
{
    struct state state;
    struct timespec delay;
    pthread_t thread;
    knurl_acl_config config;
    knurl_error error;
    const char *names[] = {"one.test", "two.test"};
    size_t i;
    memset(&state, 0, sizeof(state));
    assert(pthread_mutex_init(&state.mutex, NULL) == 0);
    assert(pthread_cond_init(&state.condition, NULL) == 0);
    knurl_acl_config_init(&config);
    config.policy = KNURL_ACL_DENY;
    config.allow.addresses = names;
    config.allow.address_count = 2;
    config.dns_refresh_ms = 1;
    config.resolve = resolve;
    config.resolve_userdata = &state;
    assert(knurl_acl_new(&config, &state.acl, &error) == KNURL_OK);
    assert(state.acl->refresh(state.acl, &error) == KNURL_OK);
    check(state.acl, "1.1.1.1", 1);
    assert(pthread_mutex_lock(&state.mutex) == 0);
    state.version = state.block = 1;
    assert(pthread_mutex_unlock(&state.mutex) == 0);
    delay.tv_sec = 0;
    delay.tv_nsec = 4000000;
    assert(nanosleep(&delay, NULL) == 0);
    assert(pthread_create(&thread, NULL, refresh, &state) == 0);
    assert(pthread_mutex_lock(&state.mutex) == 0);
    while (!state.entered)
        assert(pthread_cond_wait(&state.condition, &state.mutex) == 0);
    assert(pthread_mutex_unlock(&state.mutex) == 0);
    assert(state.acl->refresh(state.acl, &error) == KNURL_AGAIN);
    for (i = 0; i < 1000; ++i) {
        check(state.acl, "1.1.1.1", 1);
        check(state.acl, "2.2.2.2", 0);
    }
    assert(pthread_mutex_lock(&state.mutex) == 0);
    state.released = 1;
    assert(pthread_cond_signal(&state.condition) == 0);
    assert(pthread_mutex_unlock(&state.mutex) == 0);
    assert(pthread_join(thread, NULL) == 0);
    check(state.acl, "1.1.1.1", 0);
    check(state.acl, "2.2.2.2", 1);
    assert(state.acl->close(state.acl, &error) == KNURL_OK);
    assert(pthread_cond_destroy(&state.condition) == 0);
    assert(pthread_mutex_destroy(&state.mutex) == 0);
    return 0;
}
