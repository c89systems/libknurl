#define _POSIX_C_SOURCE 200809L
#include "../src/acl_internal.h"
#include <assert.h>
#include <string.h>
struct state {
    int calls, cancel, arm;
};
static int cancelled(void *data) { return ((struct state *)data)->cancel; }
static knurl_status resolve(const char *name, knurl_mmdb_address *out,
                            size_t capacity, size_t *count, void *data,
                            knurl_error *error)
{
    struct state *state = data;
    (void)name;
    assert(capacity == 64);
    ++state->calls;
    if (state->arm)
        state->cancel = 1;
    *count = 1;
    return knurl_mmdb_address_parse("192.0.2.1", out, error);
}
static void exercise(size_t names_count, int initially_cancelled)
{
    struct state state;
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_mmdb_address address;
    knurl_error error;
    const char *names[] = {"one.test", "two.test", "three.test"};
    int allowed;
    memset(&state, 0, sizeof(state));
    state.arm = 1;
    state.cancel = initially_cancelled;
    knurl_acl_config_init(&config);
    config.policy = KNURL_ACL_DENY;
    config.allow.addresses = names;
    config.allow.address_count = names_count;
    config.resolve = resolve;
    config.resolve_userdata = &state;
    assert(knurl_acl_new(&config, &acl, &error) == KNURL_OK);
    assert(knurl_acl_refresh_when(acl, cancelled, &state, &error) ==
           KNURL_TIMEOUT);
    assert(state.calls == (initially_cancelled ? 0 : 1));
    assert(knurl_mmdb_address_parse("192.0.2.1", &address, &error) == KNURL_OK);
    assert(acl->check(acl, &address, NULL, &allowed, &error) == KNURL_TIMEOUT &&
           !allowed);
    state.cancel = state.arm = state.calls = 0;
    assert(acl->refresh(acl, &error) == KNURL_OK &&
           state.calls == (int)names_count);
    assert(acl->check(acl, &address, NULL, &allowed, &error) == KNURL_OK &&
           allowed);
    assert(acl->close(acl, &error) == KNURL_OK);
}
int main(void)
{
    exercise(3, 0); /* Between names: do not continue unnecessary DNS work. */
    exercise(1,
             0); /* After final result: never publish a cancelled snapshot. */
    exercise(3, 1); /* Queued cancellation: do not invoke any resolver. */
    return 0;
}
