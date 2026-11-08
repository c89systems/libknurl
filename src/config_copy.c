#include "server_internal.h"
#include "mime_internal.h"

#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>

static void free_list(const char *const *list, size_t count)
{
    size_t i;
    if (list != NULL) {
        for (i = 0; i < count; ++i)
            free((void *)list[i]);
        free((void *)list);
    }
}

static rotmos_status copy_text(const char *input, const char **out,
                               rotmos_error *error)
{
    *out = NULL;
    if (input != NULL) {
        *out = rotmos_copy_text(input);
        if (*out == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "server_new",
                               "cannot copy configuration text");
    }
    return ROTMOS_OK;
}

static rotmos_status copy_list(const char *const *input, size_t count,
                               const char *const **out, rotmos_error *error)
{
    const char **copy;
    size_t i;
    *out = NULL;
    if (count == 0)
        return ROTMOS_OK;
    if (input == NULL || count > (size_t)-1 / sizeof(*copy))
        return rotmos_fail(error, ROTMOS_INVALID, "server_new",
                           "invalid configuration string array");
    copy = malloc(count * sizeof(*copy));
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "server_new",
                           "cannot allocate configuration string array");
    for (i = 0; i < count; ++i)
        copy[i] = NULL;
    for (i = 0; i < count; ++i) {
        if (input[i] == NULL ||
            (copy[i] = rotmos_copy_text(input[i])) == NULL) {
            free_list(copy, count);
            return rotmos_fail(
                error, input[i] == NULL ? ROTMOS_INVALID : ROTMOS_NOMEM,
                "server_new", "cannot copy configuration string array");
        }
    }
    *out = copy;
    return ROTMOS_OK;
}

static void material_free(rotmos_tls_material *material)
{
    free((void *)material->path);
    if (material->bytes.data != NULL)
        OPENSSL_cleanse((void *)material->bytes.data, material->bytes.length);
    free((void *)material->bytes.data);
    rotmos_tls_material_init(material);
}

static rotmos_status material_copy(const rotmos_tls_material *input,
                                   rotmos_tls_material *out,
                                   rotmos_error *error)
{
    void *bytes;
    rotmos_status status;
    status = copy_text(input->path, &out->path, error);
    if (status != ROTMOS_OK)
        return status;
    if (input->bytes.length != 0) {
        bytes = malloc(input->bytes.length);
        if (bytes == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "server_new",
                               "cannot copy TLS material");
        memcpy(bytes, input->bytes.data, input->bytes.length);
        out->bytes.data = bytes;
        out->bytes.length = input->bytes.length;
    }
    out->kind = input->kind;
    out->open = input->open;
    out->userdata = input->userdata;
    return ROTMOS_OK;
}

static void identity_free(rotmos_tls_identity *identity)
{
    free_list(identity->domains, identity->domain_count);
    material_free(&identity->bundle);
    material_free(&identity->certificate);
    material_free(&identity->private_key);
    material_free(&identity->extra_chain);
    material_free(&identity->client_ca);
}

void rotmos_config_cleanup(rotmos_server_config *config)
{
    size_t i;
    free((void *)config->name);
    free((void *)config->limits.body_spool_directory);
    free((void *)config->http.server_header);
    free((void *)config->http.access_log_path);
    rotmos_mime_free(config->http.mime_types, config->http.mime_type_count);
    if (config->listeners != NULL) {
        for (i = 0; i < config->listener_count; ++i) {
            free((void *)config->listeners[i].name);
            free((void *)config->listeners[i].bind);
        }
        free((void *)config->listeners);
    }
    if (config->tls.identities != NULL) {
        for (i = 0; i < config->tls.identity_count; ++i)
            identity_free((rotmos_tls_identity *)&config->tls.identities[i]);
        free((void *)config->tls.identities);
    }
    free((void *)config->tls.cipher_list);
    free((void *)config->tls.acme.email);
    free((void *)config->tls.acme.directory_url);
    free((void *)config->tls.acme.runtime_directory);
    free((void *)config->tls.acme.ca_file);
    free((void *)config->tls.acme.ca_directory);
    free_list(config->client_ip.trusted_proxies,
              config->client_ip.trusted_proxy_count);
    free((void *)config->autoblock.status_rules);
    if (config->autoblock.event_rules != NULL) {
        for (i = 0; i < config->autoblock.event_rule_count; ++i)
            free((void *)config->autoblock.event_rules[i].name);
        free((void *)config->autoblock.event_rules);
    }
}

rotmos_status rotmos_config_copy(const rotmos_server_config *input,
                                 rotmos_server_config *out, rotmos_error *error)
{
    rotmos_listener_config *listeners;
    rotmos_tls_identity *identities;
    rotmos_autoblock_status_rule *statuses;
    rotmos_autoblock_event_rule *events;
    rotmos_status status;
    size_t i;
    *out = *input;
    out->name = NULL;
    out->listeners = NULL;
    out->limits.body_spool_directory = NULL;
    out->http.server_header = out->http.access_log_path = NULL;
    out->http.mime_types = NULL;
    out->tls.identities = NULL;
    out->tls.cipher_list = NULL;
    out->tls.acme.email = out->tls.acme.directory_url = NULL;
    out->tls.acme.runtime_directory = NULL;
    out->tls.acme.ca_file = out->tls.acme.ca_directory = NULL;
    out->client_ip.trusted_proxies = NULL;
    out->autoblock.status_rules = NULL;
    out->autoblock.event_rules = NULL;
    status = ROTMOS_OK;
#define COPY_TEXT(field)                                                       \
    do {                                                                       \
        status = copy_text(input->field, &out->field, error);                  \
        if (status != ROTMOS_OK)                                               \
            goto failed;                                                       \
    } while (0)
    COPY_TEXT(name);
    COPY_TEXT(limits.body_spool_directory);
    COPY_TEXT(http.server_header);
    COPY_TEXT(http.access_log_path);
    COPY_TEXT(tls.cipher_list);
    COPY_TEXT(tls.acme.email);
    COPY_TEXT(tls.acme.directory_url);
    COPY_TEXT(tls.acme.runtime_directory);
    COPY_TEXT(tls.acme.ca_file);
    COPY_TEXT(tls.acme.ca_directory);
#undef COPY_TEXT
    status =
        rotmos_mime_copy(input->http.mime_types, input->http.mime_type_count,
                         &out->http.mime_types, error);
    if (status != ROTMOS_OK)
        goto failed;
    status = copy_list(input->client_ip.trusted_proxies,
                       input->client_ip.trusted_proxy_count,
                       &out->client_ip.trusted_proxies, error);
    if (status != ROTMOS_OK)
        goto failed;
    if (input->listener_count != 0) {
        listeners = malloc(input->listener_count * sizeof(*listeners));
        if (listeners == NULL)
            goto no_memory;
        out->listeners = listeners;
        for (i = 0; i < input->listener_count; ++i) {
            listeners[i] = input->listeners[i];
            listeners[i].name = listeners[i].bind = NULL;
        }
        for (i = 0; i < input->listener_count; ++i) {
            status =
                copy_text(input->listeners[i].name, &listeners[i].name, error);
            if (status == ROTMOS_OK)
                status = copy_text(input->listeners[i].bind, &listeners[i].bind,
                                   error);
            if (status != ROTMOS_OK)
                goto failed;
        }
    }
    if (input->tls.identity_count != 0) {
        identities = malloc(input->tls.identity_count * sizeof(*identities));
        if (identities == NULL)
            goto no_memory;
        out->tls.identities = identities;
        for (i = 0; i < input->tls.identity_count; ++i)
            rotmos_tls_identity_init(&identities[i]);
        for (i = 0; i < input->tls.identity_count; ++i) {
            identities[i].mode = input->tls.identities[i].mode;
            identities[i].domain_count = input->tls.identities[i].domain_count;
            identities[i].require_client_certificate =
                input->tls.identities[i].require_client_certificate;
            status = copy_list(input->tls.identities[i].domains,
                               identities[i].domain_count,
                               &identities[i].domains, error);
            if (status != ROTMOS_OK)
                goto failed;
#define COPY_MATERIAL(field)                                                   \
    do {                                                                       \
        status = material_copy(&input->tls.identities[i].field,                \
                               &identities[i].field, error);                   \
        if (status != ROTMOS_OK)                                               \
            goto failed;                                                       \
    } while (0)
            COPY_MATERIAL(bundle);
            COPY_MATERIAL(certificate);
            COPY_MATERIAL(private_key);
            COPY_MATERIAL(extra_chain);
            COPY_MATERIAL(client_ca);
#undef COPY_MATERIAL
        }
    }
    if (input->autoblock.status_rule_count != 0) {
        statuses =
            malloc(input->autoblock.status_rule_count * sizeof(*statuses));
        if (statuses == NULL)
            goto no_memory;
        memcpy(statuses, input->autoblock.status_rules,
               input->autoblock.status_rule_count * sizeof(*statuses));
        out->autoblock.status_rules = statuses;
    }
    if (input->autoblock.event_rule_count != 0) {
        events = malloc(input->autoblock.event_rule_count * sizeof(*events));
        if (events == NULL)
            goto no_memory;
        out->autoblock.event_rules = events;
        for (i = 0; i < input->autoblock.event_rule_count; ++i) {
            events[i].name = NULL;
            events[i].threshold = input->autoblock.event_rules[i].threshold;
        }
        for (i = 0; i < input->autoblock.event_rule_count; ++i) {
            status = copy_text(input->autoblock.event_rules[i].name,
                               &events[i].name, error);
            if (status != ROTMOS_OK)
                goto failed;
        }
    }
    return ROTMOS_OK;
no_memory:
    status = rotmos_fail(error, ROTMOS_NOMEM, "server_new",
                         "cannot allocate configuration arrays");
failed:
    rotmos_config_cleanup(out);
    return status;
}
