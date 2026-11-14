#include "acl_internal.h"
#include "loop_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void knurl_acl_config_init(knurl_acl_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
static int valid_rules(const knurl_acl_rules *rules)
{
    size_t i;
    if (rules->address_count > 1024 || rules->country_count > 1024 ||
        (rules->address_count && rules->addresses == NULL) ||
        (rules->country_count && rules->countries == NULL))
        return 0;
    for (i = 0; i < rules->country_count; ++i) {
        const char *code = rules->countries[i];
        if (code == NULL || strlen(code) != 2 || code[0] < 'A' ||
            code[0] > 'Z' || code[1] < 'A' || code[1] > 'Z')
            return 0;
    }
    return 1;
}
static int needs_country(const knurl_acl_config *config)
{
    return config->allow.country_count || config->deny.country_count ||
           config->allow.unknown || config->deny.unknown;
}
knurl_status knurl_acl_new(const knurl_acl_config *input, knurl_acl **out,
                           knurl_error *error)
{
    static const knurl_acl methods = {knurl_acl_retain, knurl_acl_refresh,
                                      knurl_acl_check, knurl_acl_close, NULL};
    knurl_acl_config defaults;
    struct knurl_acl_impl *impl;
    knurl_acl *acl;
    const knurl_acl_rules *rules;
    size_t i, j, at = 0;
    knurl_status status = KNURL_NOMEM;
    if (out != NULL)
        *out = NULL;
    knurl_acl_config_init(&defaults);
    if (input == NULL)
        input = &defaults;
    if (out == NULL || input->policy < KNURL_ACL_ALLOW ||
        input->policy > KNURL_ACL_DENY || !valid_rules(&input->allow) ||
        !valid_rules(&input->deny) || input->dns_refresh_ms < 0 ||
        input->dns_timeout_ms < 0 || input->dns_max_addresses > 1024 ||
        input->allow.address_count + input->deny.address_count > 1024 ||
        (input->country_database && input->country_provider) ||
        (input->country_database && input->country_database[0] == 0) ||
        (input->max_heap_bytes &&
         input->max_heap_bytes < KNURL_MMDB_HEAP_MIN) ||
        (needs_country(input) && !input->country_database &&
         !input->country_provider))
        return knurl_fail(error, KNURL_INVALID, "acl.new",
                          "valid ACL policy, bounded rules, DNS settings and "
                          "country database required");
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL)
        return knurl_fail(error, KNURL_NOMEM, "acl.new", "allocation failed");
    if (pthread_mutex_init(&impl->mutex, NULL) != 0) {
        free(impl);
        return knurl_fail(error, KNURL_BACKEND, "acl.new",
                          "ACL lock initialization failed");
    }
    if (pthread_mutex_init(&impl->refresh_mutex, NULL) != 0) {
        (void)pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return knurl_fail(error, KNURL_BACKEND, "acl.new",
                          "refresh lock initialization failed");
    }
    acl = &impl->receiver;
    *acl = methods;
    acl->impl = impl;
    impl->references = 1;
    impl->config = *input;
    impl->config.allow.addresses = impl->config.deny.addresses = NULL;
    impl->config.allow.countries = impl->config.deny.countries = NULL;
    impl->config.country_database = NULL;
    if (!impl->config.dns_max_addresses)
        impl->config.dns_max_addresses = 64;
    if (!impl->config.dns_timeout_ms)
        impl->config.dns_timeout_ms = 10000;
    if (!impl->config.resolve)
        impl->config.resolve = knurl_acl_resolve_system;
    impl->database = input->country_provider;
    if (input->country_database != NULL &&
        (impl->config.country_database =
             knurl_copy_text(input->country_database)) == NULL)
        goto failed;
    impl->entry_count = input->allow.address_count + input->deny.address_count;
    if (impl->entry_count &&
        (impl->entries = calloc(impl->entry_count, sizeof(*impl->entries))) ==
            NULL)
        goto failed;
    for (i = 0; i < 2; ++i) {
        rules = i ? &input->deny : &input->allow;
        if (rules->country_count) {
            impl->countries[i] = malloc(rules->country_count * 2 + 1);
            if (impl->countries[i] == NULL)
                goto failed;
            for (j = 0; j < rules->country_count; ++j)
                memcpy(impl->countries[i] + j * 2, rules->countries[j], 2);
            impl->countries[i][rules->country_count * 2] = 0;
        }
        for (j = 0; j < rules->address_count; ++j, ++at) {
            impl->entries[at].deny = (int)i;
            if (!knurl_ip_network_parse(rules->addresses[j],
                                        &impl->entries[at].network)) {
                if (!knurl_ip_hostname(rules->addresses[j])) {
                    status = KNURL_INVALID;
                    goto failed;
                }
                impl->entries[at].hostname =
                    knurl_copy_text(rules->addresses[j]);
                if (impl->entries[at].hostname == NULL)
                    goto failed;
            }
        }
    }
    *out = acl;
    knurl_error_clear(error);
    return KNURL_OK;
failed:
    (void)knurl_acl_close(acl, NULL);
    return knurl_fail(
        error, status, "acl.new",
        status == KNURL_INVALID
            ? "address rule must be a numeric address/CIDR or DNS hostname"
            : "cannot copy ACL configuration");
}
knurl_status knurl_acl_retain(knurl_acl *acl, knurl_error *error)
{
    if (acl == NULL)
        return knurl_fail(error, KNURL_INVALID, "acl.retain", "ACL required");
    (void)pthread_mutex_lock(&acl->impl->mutex);
    if (acl->impl->references == (size_t)-1) {
        (void)pthread_mutex_unlock(&acl->impl->mutex);
        return knurl_fail(error, KNURL_OVERFLOW, "acl.retain",
                          "reference count overflow");
    }
    ++acl->impl->references;
    (void)pthread_mutex_unlock(&acl->impl->mutex);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_acl_close(knurl_acl *acl, knurl_error *error)
{
    struct knurl_acl_impl *impl;
    size_t i;
    int last;
    if (acl != NULL) {
        impl = acl->impl;
        (void)pthread_mutex_lock(&impl->mutex);
        last = --impl->references == 0;
        (void)pthread_mutex_unlock(&impl->mutex);
        if (last) {
            if (impl->entries != NULL)
                for (i = 0; i < impl->entry_count; ++i)
                    free(impl->entries[i].hostname);
            free(impl->entries);
            free(impl->resolved);
            free(impl->countries[0]);
            free(impl->countries[1]);
            free((void *)impl->config.country_database);
            if (impl->own_database)
                (void)knurl_mmdb_close(impl->database, NULL);
            (void)pthread_mutex_destroy(&impl->refresh_mutex);
            (void)pthread_mutex_destroy(&impl->mutex);
            free(impl);
        }
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
static int due_locked(struct knurl_acl_impl *impl, knurl_u64 now)
{
    int order = 0;
    if (!impl->attempted)
        return 1;
    if (!impl->config.dns_refresh_ms)
        return !impl->prepared;
    (void)knurl_u64_compare(now, impl->due, &order, NULL);
    return order >= 0;
}
int knurl_acl_due(knurl_acl *acl)
{
    knurl_u64 now;
    int due;
    if (knurl_loop_monotonic(&now, NULL) != KNURL_OK)
        return 1;
    (void)pthread_mutex_lock(&acl->impl->mutex);
    due = due_locked(acl->impl, now);
    (void)pthread_mutex_unlock(&acl->impl->mutex);
    return due;
}
void knurl_acl_unavailable(knurl_acl *acl, knurl_status status,
                           const char *message)
{
    (void)pthread_mutex_lock(&acl->impl->mutex);
    (void)knurl_fail(&acl->impl->failure, status, "acl.refresh", message);
    (void)pthread_mutex_unlock(&acl->impl->mutex);
}
knurl_status knurl_acl_refresh_when(knurl_acl *acl, int (*cancelled)(void *),
                                    void *userdata, knurl_error *error)
{
    struct knurl_acl_impl *impl;
    struct knurl_acl_resolved *resolved = NULL, *old;
    size_t count = 0;
    knurl_error failure;
    knurl_mmdb_file_config config;
    knurl_u64 now;
    knurl_status status;
    int lock;
    if (acl == NULL)
        return knurl_fail(error, KNURL_INVALID, "acl.refresh", "ACL required");
    impl = acl->impl;
    lock = pthread_mutex_trylock(&impl->refresh_mutex);
    if (lock != 0) {
        knurl_error_clear(error);
        return lock == EBUSY ? KNURL_AGAIN
                             : knurl_fail(error, KNURL_BACKEND, "acl.refresh",
                                          "refresh lock failed");
    }
    knurl_error_clear(&failure);
    status = knurl_loop_monotonic(&now, &failure);
    (void)pthread_mutex_lock(&impl->mutex);
    if (status == KNURL_OK && !due_locked(impl, now)) {
        (void)pthread_mutex_unlock(&impl->mutex);
        (void)pthread_mutex_unlock(&impl->refresh_mutex);
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    impl->attempted = 1;
    if (status == KNURL_OK)
        status = knurl_loop_deadline(now, impl->config.dns_refresh_ms,
                                     &impl->due, &failure);
    (void)pthread_mutex_unlock(&impl->mutex);
    if (status == KNURL_OK && impl->database == NULL &&
        impl->config.country_database) {
        knurl_mmdb_file_config_init(&config);
        config.max_heap_bytes = impl->config.max_heap_bytes;
        status = knurl_mmdb_open(impl->config.country_database, &config,
                                 &impl->database, &failure);
        if (status == KNURL_OK)
            impl->own_database = 1;
    }
    if (status == KNURL_OK)
        status = knurl_acl_resolve_all(acl, &resolved, &count, cancelled,
                                       userdata, &failure);
    if (status == KNURL_OK)
        knurl_error_clear(&failure);
    else if (failure.status != status)
        (void)knurl_fail(&failure, status, "acl.refresh",
                         "ACL preparation failed");
    (void)pthread_mutex_lock(&impl->mutex);
    impl->failure = failure;
    if (status == KNURL_OK && cancelled != NULL && cancelled(userdata)) {
        status = knurl_fail(&failure, KNURL_TIMEOUT, "acl.refresh",
                            "DNS refresh cancelled or deadline exceeded");
        impl->failure = failure;
    }
    if (status == KNURL_OK) {
        old = impl->resolved;
        impl->resolved = resolved;
        impl->resolved_count = count;
        impl->prepared = 1;
        resolved = old;
    }
    (void)pthread_mutex_unlock(&impl->mutex);
    free(resolved);
    (void)pthread_mutex_unlock(&impl->refresh_mutex);
    if (error != NULL)
        *error = failure;
    return status;
}
knurl_status knurl_acl_refresh(knurl_acl *acl, knurl_error *error)
{
    return knurl_acl_refresh_when(acl, NULL, NULL, error);
}
static int country_match(const char *codes, size_t count, const char code[3])
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (memcmp(codes + i * 2, code, 2) == 0)
            return 1;
    return 0;
}
knurl_status knurl_acl_check(knurl_acl *acl, const knurl_mmdb_address *peer,
                             const knurl_mmdb_address *effective, int *allowed,
                             knurl_error *error)
{
    struct knurl_acl_impl *impl;
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_status status = KNURL_OK;
    size_t i, low, high, middle;
    int matches[2] = {0, 0};
    if (allowed != NULL)
        *allowed = 0;
    if (acl == NULL || allowed == NULL ||
        !knurl_ip_normalize(
            acl->impl->config.use_peer || effective == NULL ? peer : effective,
            &address))
        return knurl_fail(error, KNURL_INVALID, "acl.check",
                          "ACL, numeric client and output required");
    impl = acl->impl;
    (void)pthread_mutex_lock(&impl->mutex);
    if (!impl->prepared || impl->failure.status != KNURL_OK) {
        if (impl->failure.status != KNURL_OK) {
            status = impl->failure.status;
            if (error != NULL)
                *error = impl->failure;
        } else
            status = knurl_fail(error, KNURL_STATE, "acl.check",
                                "ACL is not prepared");
        goto done;
    }
    for (i = 0; i < impl->entry_count; ++i)
        if (impl->entries[i].hostname == NULL &&
            knurl_ip_contains(&impl->entries[i].network, &address))
            matches[impl->entries[i].deny] = 1;
    low = 0;
    high = impl->resolved_count;
    while (low < high) {
        middle = low + (high - low) / 2;
        if (knurl_ip_compare(&impl->resolved[middle].address, &address) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    for (i = low; i < impl->resolved_count &&
                  knurl_ip_compare(&impl->resolved[i].address, &address) == 0;
         ++i)
        matches[impl->resolved[i].deny] = 1;
    if (matches[1])
        goto success;
    if (needs_country(&impl->config)) {
        status = knurl_mmdb_lookup(impl->database, &address, &country, error);
        if (status != KNURL_OK && status != KNURL_EOF)
            goto done;
        if (status == KNURL_EOF) {
            matches[0] |= impl->config.allow.unknown != 0;
            matches[1] |= impl->config.deny.unknown != 0;
        } else {
            matches[0] |=
                country_match(impl->countries[0],
                              impl->config.allow.country_count, country.code);
            matches[1] |=
                country_match(impl->countries[1],
                              impl->config.deny.country_count, country.code);
        }
    }
success:
    *allowed =
        !matches[1] && (matches[0] || impl->config.policy == KNURL_ACL_ALLOW);
    status = KNURL_OK;
    knurl_error_clear(error);
done:
    (void)pthread_mutex_unlock(&impl->mutex);
    return status;
}
