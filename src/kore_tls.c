#include "kore_internal.h"
#include "response_internal.h"
#include "acme.h"

#include <arpa/inet.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void rotmos_kore_listener_domains(struct rotmos_runtime *runtime,
                                  struct kore_server *server,
                                  const rotmos_listener_config *listener)
{
    const rotmos_tls_identity *identity;
    struct kore_domain *domain;
    struct kore_route *route;
    size_t count;
    size_t i;
    identity =
        listener->kind == ROTMOS_LISTENER_HTTPS
            ? &runtime->server->config.tls.identities[listener->identity_index]
            : NULL;
    count = identity != NULL ? identity->domain_count : 1;
    for (i = 0; i < count; ++i) {
        domain = kore_domain_new(identity != NULL ? identity->domains[i] : "*");
        if (!kore_domain_attach(domain, server))
            fatal("cannot attach listener domain");
        if (runtime->server->config.http.access_log_path != NULL)
            domain->accesslog = KORE_ACCESSLOG_EXTERNAL;
        if (identity != NULL) {
            runtime->tls_bindings[runtime->tls_binding_count].domain = domain;
            runtime->tls_bindings[runtime->tls_binding_count++].identity =
                listener->identity_index;
            if (identity->mode == ROTMOS_TLS_ACME) {
                domain->acme = 1;
                ++acme_domains;
                kore_acme_get_paths(domain->domain, &domain->certkey,
                                    &domain->certfile);
            } else {
                if (!SSL_CTX_up_ref(
                        runtime->tls.contexts[listener->identity_index]))
                    fatal("cannot retain TLS identity context");
                kore_library_tls_context_install(
                    domain, runtime->tls.contexts[listener->identity_index]);
            }
        }
        route = kore_route_create(domain, "^/", HANDLER_TYPE_DYNAMIC);
        if (route == NULL)
            fatal("cannot install public router adapter");
        route->func = kore_strdup("rotmos dispatcher");
        route->rcall = kore_runtime_native(&runtime->callbacks);
        route->on_prebody = rotmos_kore_prebody;
        route->on_free = kore_runtime_native(&runtime->callbacks);
    }
}

static int authority(const char *input, char *host)
{
    const char *end;
    const char *port;
    const char *cursor;
    unsigned char address[16];
    unsigned long number;
    size_t length;
    if (input == NULL || *input == '\0')
        return 0;
    port = NULL;
    if (*input == '[') {
        end = strchr(input, ']');
        if (end == NULL || end == input + 1 || (size_t)(end - input) >= 253)
            return 0;
        length = (size_t)(end - input - 1);
        memcpy(host, input + 1, length);
        host[length] = '\0';
        if (inet_pton(AF_INET6, host, address) != 1)
            return 0;
        ++end;
        if (*end != '\0') {
            if (*end != ':')
                return 0;
            port = end + 1;
        }
    } else {
        end = strchr(input, ':');
        if (end == NULL)
            end = input + strlen(input);
        else
            port = end + 1;
        if (end == input || (size_t)(end - input) > 253)
            return 0;
        for (cursor = input; cursor < end; ++cursor) {
            if (!((*cursor >= 'a' && *cursor <= 'z') ||
                  (*cursor >= 'A' && *cursor <= 'Z') ||
                  (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
                  *cursor == '-' || *cursor == '_' || *cursor == '~' ||
                  strchr("!$&'()*+,;=", *cursor) != NULL))
                return 0;
        }
    }
    if (port != NULL) {
        number = 0;
        if (*port == '\0')
            return 0;
        for (cursor = port; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9' || number > 6553)
                return 0;
            number = number * 10 + (unsigned long)(*cursor - '0');
            if (number > 65535)
                return 0;
        }
        if (number == 0)
            return 0;
    }
    length = (size_t)(end - input);
    memcpy(host, input, length);
    host[length] = '\0';
    return 1;
}

static int allowed(const rotmos_tls_identity *identity, const char *host)
{
    char pattern[254];
    char name[254];
    size_t i;
    size_t j;
    unsigned char byte;
    for (j = 0; host[j] != '\0'; ++j) {
        byte = (unsigned char)host[j];
        name[j] = (char)(byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte);
    }
    name[j] = '\0';
    for (i = 0; i < identity->domain_count; ++i) {
        for (j = 0; identity->domains[i][j] != '\0'; ++j) {
            byte = (unsigned char)identity->domains[i][j];
            pattern[j] =
                (char)(byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte);
        }
        pattern[j] = '\0';
        if (strcmp(pattern, name) == 0 || fnmatch(pattern, name, 0) == 0)
            return 1;
    }
    return 0;
}

int rotmos_kore_redirect(rotmos_request *request)
{
    const rotmos_listener_config *listener;
    rotmos_u64 length_zero = {0, 0};
    rotmos_status result;
    char host[254];
    char port[8];
    char *location;
    const char *target;
    size_t length;
    size_t bound;
    int status;
    listener = &request->server->config.listeners[request->listener];
    status = 400;
    location = NULL;
    if (authority(request->authority.data, host) &&
        allowed(
            &request->server->config.tls.identities[listener->identity_index],
            host)) {
        port[0] = '\0';
        if (listener->redirect_https_port != 443)
            (void)snprintf(port, sizeof(port), ":%u",
                           (unsigned int)listener->redirect_https_port);
        target = request->target.length != 0 &&
                         ((const char *)request->target.data)[0] == '/'
                     ? request->target.data
                     : "/";
        bound = request->server->config.limits.max_response_header_bytes;
        if (bound == 0)
            bound = (size_t)-1;
        length = 8 + strlen(host) + strlen(port);
        if (length >= bound || strlen(target) >= bound - length)
            status = 414;
        else {
            length += strlen(target);
            location = malloc(length + 1);
            if (location == NULL)
                status = 500;
            else {
                (void)snprintf(location, length + 1, "https://%s%s%s", host,
                               port, target);
                result = rotmos_response_header_add(request->response,
                                                    "location", location, NULL);
                status = result == ROTMOS_OK ? 308 : 500;
            }
        }
    }
    free(location);
    request->response->close_connection = 1;
    result = rotmos_response_status(request->response, status, NULL);
    if (result == ROTMOS_OK)
        result =
            rotmos_response_known_length(request->response, length_zero, NULL);
    if (result == ROTMOS_OK)
        result = rotmos_response_begin(request->response, NULL);
    if (result == ROTMOS_OK)
        result = rotmos_response_end(request->response, NULL, 0, NULL);
    if (result != ROTMOS_OK)
        rotmos_kore_request_fail(request, result, 500);
    return KORE_RESULT_RETRY;
}
