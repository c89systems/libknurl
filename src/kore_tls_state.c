#define _GNU_SOURCE 1
#include "kore_internal.h"

#include <sys/mman.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

rotmos_status rotmos_kore_tls_registry(struct rotmos_runtime *runtime,
                                       rotmos_error *error)
{
    const rotmos_server_config *config;
    size_t count;
    size_t slots;
    size_t i;
    config = &runtime->server->config;
    count = 0;
    for (i = 0; i < config->listener_count; ++i)
        if (config->listeners[i].kind == ROTMOS_LISTENER_HTTPS) {
            slots = config->tls.identities[config->listeners[i].identity_index]
                        .domain_count;
            if (slots > (size_t)-1 - count)
                return rotmos_fail(error, ROTMOS_OVERFLOW, "tls_registry",
                                   "TLS domain registry exceeds size range");
            count += slots;
        }
    if (config->tls.identity_count == 0)
        return ROTMOS_OK;
    runtime->tls_observed =
        calloc(config->tls.identity_count, sizeof(*runtime->tls_observed));
    if (runtime->tls_observed == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "tls_registry",
                           "cannot allocate TLS identity observations");
    for (i = 0; i < config->tls.identity_count; ++i)
        runtime->tls_observed[i] = -1;
    if (count == 0)
        return ROTMOS_OK;
    if (count > (size_t)-1 / sizeof(*runtime->tls_bindings) ||
        count > (size_t)-1 / sizeof(int) / config->workers.count)
        return rotmos_fail(error, ROTMOS_OVERFLOW, "tls_registry",
                           "TLS worker registry exceeds size range");
    slots = count * config->workers.count;
    runtime->tls_bindings = calloc(count, sizeof(*runtime->tls_bindings));
    runtime->tls_installed_bytes = slots * sizeof(int);
    runtime->tls_seen = calloc(slots, sizeof(int));
    runtime->tls_installed =
        mmap(NULL, runtime->tls_installed_bytes, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (runtime->tls_installed == MAP_FAILED)
        runtime->tls_installed = NULL;
    if (runtime->tls_bindings == NULL || runtime->tls_observed == NULL ||
        runtime->tls_installed == NULL || runtime->tls_seen == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "tls_registry",
                           "cannot allocate TLS identity observations");
    return ROTMOS_OK;
}

void rotmos_kore_tls_clear(struct rotmos_runtime *runtime)
{
    if (runtime->tls_installed != NULL)
        (void)munmap(runtime->tls_installed, runtime->tls_installed_bytes);
    runtime->tls_installed = NULL;
    free(runtime->tls_bindings);
    free(runtime->tls_observed);
    free(runtime->tls_seen);
    runtime->tls_bindings = NULL;
    runtime->tls_observed = NULL;
    runtime->tls_seen = NULL;
}

void rotmos_kore_tls_worker(struct rotmos_runtime *runtime, size_t index,
                            int starting)
{
    size_t i;
    size_t offset;
    int usable;
    offset = index * runtime->tls_binding_count;
    for (i = 0; i < runtime->tls_binding_count; ++i) {
        usable =
            starting && runtime->server->config.tls
                                .identities[runtime->tls_bindings[i].identity]
                                .mode == ROTMOS_TLS_MANUAL;
        __atomic_store_n(&runtime->tls_installed[offset + i], usable,
                         __ATOMIC_RELEASE);
    }
}

void rotmos_kore_tls_updated(void *userdata, struct kore_domain *domain,
                             int usable)
{
    struct rotmos_runtime *runtime;
    const rotmos_tls_config *config;
    size_t i;
    size_t identity;
    size_t offset;
    int revision;
    runtime = userdata;
    config = &runtime->server->config.tls;
    for (i = 0; i < runtime->tls_binding_count; ++i) {
        if (runtime->tls_bindings[i].domain != domain)
            continue;
        identity = runtime->tls_bindings[i].identity;
        if (rotmos_tls_policy_apply(
                runtime->tls.contexts[identity], domain->tls_ctx,
                config->cipher_list != NULL ? config->cipher_list
                                            : KORE_DEFAULT_CIPHER_LIST,
                &runtime->error) != ROTMOS_OK) {
            runtime->status = runtime->error.status;
            rotmos_kore_child_failure(runtime, &runtime->error);
            fatal("cannot apply prepared ACME identity policy");
        }
        if (usable) {
            offset = (worker->id - 1U) * runtime->tls_binding_count + i;
            revision = __atomic_load_n(&runtime->tls_installed[offset],
                                       __ATOMIC_RELAXED);
            if (revision == INT_MAX)
                fatal("TLS installation observation count exhausted");
            __atomic_store_n(&runtime->tls_installed[offset], revision + 1,
                             __ATOMIC_RELEASE);
        }
        break;
    }
}

rotmos_status rotmos_server_tls_identity_state(const rotmos_server *server,
                                               size_t index,
                                               rotmos_tls_identity_state *out,
                                               rotmos_error *error)
{
    const struct rotmos_runtime *runtime;
    size_t i;
    size_t j;
    int found;
    if (server == NULL || out == NULL ||
        index >= server->config.tls.identity_count)
        return rotmos_fail(
            error, ROTMOS_INVALID, "tls_identity_state",
            "provide server, output and configured identity index");
    runtime = server->runtime;
    if (runtime == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "tls_identity_state",
                           "TLS identity state belongs to the active run");
    *out = ROTMOS_TLS_PENDING;
    found = 0;
    for (i = 0; i < runtime->tls_binding_count; ++i) {
        if (runtime->tls_bindings[i].identity != index)
            continue;
        found = 1;
        for (j = 0; j < server->config.workers.count; ++j)
            if (!__atomic_load_n(
                    &runtime->tls_installed[j * runtime->tls_binding_count + i],
                    __ATOMIC_ACQUIRE)) {
                rotmos_error_clear(error);
                return ROTMOS_OK;
            }
    }
    if (found)
        *out = ROTMOS_TLS_USABLE;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_kore_tls_observe(struct rotmos_runtime *runtime,
                             const rotmos_error *failure)
{
    rotmos_tls_identity_event event;
    const rotmos_tls_identity *identity;
    size_t i;
    size_t failed_identity;
    size_t j;
    size_t k;
    size_t offset;
    int changed;
    int revision;
    if (!runtime->ready || runtime->tls_observed == NULL)
        return;
    failed_identity = (size_t)-1;
    if (failure != NULL &&
        __atomic_load_n(&runtime->shared->acme_identity_valid,
                        __ATOMIC_ACQUIRE))
        failed_identity =
            __atomic_load_n(&runtime->shared->acme_identity, __ATOMIC_ACQUIRE);
    for (i = 0; i < runtime->server->config.tls.identity_count; ++i) {
        memset(&event, 0, sizeof(event));
        event.identity_index = i;
        (void)rotmos_server_tls_identity_state(runtime->server, i, &event.state,
                                               NULL);
        identity = &runtime->server->config.tls.identities[i];
        changed = 0;
        for (j = 0; j < runtime->tls_binding_count; ++j)
            if (runtime->tls_bindings[j].identity == i)
                for (k = 0; k < runtime->server->config.workers.count; ++k) {
                    offset = k * runtime->tls_binding_count + j;
                    revision = __atomic_load_n(&runtime->tls_installed[offset],
                                               __ATOMIC_ACQUIRE);
                    if (revision != runtime->tls_seen[offset])
                        changed = 1;
                    runtime->tls_seen[offset] = revision;
                }
        if (failure != NULL && identity->mode == ROTMOS_TLS_ACME &&
            (failed_identity == (size_t)-1 || failed_identity == i)) {
            event.error = *failure;
            if (failed_identity == i) {
                j = __atomic_load_n(&runtime->shared->acme_domain,
                                    __ATOMIC_ACQUIRE);
                event.domain = rotmos_view(identity->domains[j]);
            }
        }
        if (runtime->tls_observed[i] == (int)event.state &&
            event.error.status == ROTMOS_OK && !changed)
            continue;
        runtime->tls_observed[i] = (int)event.state;
        if (runtime->server->config.callbacks.on_tls_identity != NULL)
            runtime->server->config.callbacks.on_tls_identity(
                runtime->server, &event,
                runtime->server->config.callbacks.userdata);
    }
}
