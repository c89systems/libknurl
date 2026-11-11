/* Trusted-chain resolution lifted from Vectis's Kore bridge. */
#define _POSIX_C_SOURCE 200809L
#include "policy_internal.h"
#include "server_internal.h"
#include <arpa/inet.h>
#include <string.h>
static int address(const char *value, unsigned char bytes[16])
{
    if (inet_pton(AF_INET, value, bytes) == 1)
        return 4;
    if (inet_pton(AF_INET6, value, bytes) == 1)
        return 16;
    return 0;
}
static int trusted(const knurl_client_ip_config *config, const char *value)
{
    unsigned char left[16], right[16];
    size_t i;
    int n = address(value, left);
    if (n == 0)
        return 0;
    for (i = 0; i < config->trusted_proxy_count; ++i)
        if (address(config->trusted_proxies[i], right) == n &&
            !memcmp(left, right, (size_t)n))
            return 1;
    return 0;
}
static int token(const char *start, const char *end, char out[46])
{
    size_t n;
    unsigned char bytes[16];
    while (start < end && (*start == ' ' || *start == '\t'))
        ++start;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    n = (size_t)(end - start);
    if (n == 0 || n >= 46)
        return 0;
    memcpy(out, start, n);
    out[n] = 0;
    return address(out, bytes) != 0;
}
static void via_add(knurl_autoblock_event *event, const char *ip)
{
    if (event->via_count < KNURL_AUTOBLOCK_MAX_VIA)
        strcpy(event->via[event->via_count++], ip);
    else
        event->via_truncated = 1;
}
static int forwarded(const knurl_client_ip_config *config, knurl_bytes header,
                     char out[46], knurl_autoblock_event *event)
{
    const char *start, *end, *value = header.data;
    if (value == NULL || header.length == 0)
        return 0;
    end = value + header.length;
    while (end > value) {
        start = end;
        while (start > value && start[-1] != ',')
            --start;
        if (!token(start, end, out))
            return 0;
        if (!trusted(config, out) || start == value)
            return 1;
        if (event != NULL)
            via_add(event, out);
        end = start - 1;
    }
    return 0;
}
knurl_status knurl_request_identity(knurl_request *r, knurl_error *error)
{
    const knurl_server_config *config = &r->impl->server->impl->config;
    knurl_bytes header;
    knurl_status status;
    size_t n = 0, i;
    unsigned char bytes[16];
    char resolved[46];
    int found = 0;
    r->impl->effective_address = r->impl->peer.address;
    r->impl->address_source = KNURL_CLIENT_ADDRESS_PEER;
    if (r->impl->peer.address.data != NULL &&
        trusted(&config->client_ip, r->impl->peer.address.data)) {
        if (knurl_request_header_first(r, "x-forwarded-for", &header, NULL) ==
            KNURL_OK)
            found = forwarded(&config->client_ip, header,
                              r->impl->effective_storage, NULL);
        if (found)
            r->impl->address_source = KNURL_CLIENT_ADDRESS_FORWARDED_FOR;
        if (!found && knurl_request_header_first(r, "x-real-ip", &header,
                                                 NULL) == KNURL_OK) {
            found =
                token(header.data, (const char *)header.data + header.length,
                      r->impl->effective_storage);
            if (found)
                r->impl->address_source = KNURL_CLIENT_ADDRESS_REAL_IP;
        }
        if (found)
            r->impl->effective_address = knurl_view(r->impl->effective_storage);
    }
    if (config->client_ip.resolve != NULL) {
        status = config->client_ip.resolve(r, resolved, sizeof(resolved), &n,
                                           config->client_ip.userdata, error);
        if (status != KNURL_OK)
            return status;
        if (n == 0 || n >= sizeof(r->impl->effective_storage) ||
            memchr(resolved, 0, n) != NULL)
            return knurl_fail(error, KNURL_INVALID, "client_address",
                              "resolver returned an invalid address length");
        resolved[n] = 0;
        if (address(resolved, bytes) == 0)
            return knurl_fail(
                error, KNURL_INVALID, "client_address",
                "resolver must return a numeric IPv4 or IPv6 address");
        r->impl->address_source = KNURL_CLIENT_ADDRESS_RESOLVER;
        memcpy(r->impl->effective_storage, resolved, n + 1);
        r->impl->effective_address = knurl_view(r->impl->effective_storage);
    }
    if (config->correlation_id.resolve != NULL) {
        n = 0;
        status = config->correlation_id.resolve(
            r, r->impl->correlation_storage,
            sizeof(r->impl->correlation_storage), &n,
            config->correlation_id.userdata, error);
        if (status != KNURL_OK)
            return status;
        if (n > KNURL_CORRELATION_ID_MAX)
            return knurl_fail(error, KNURL_LIMIT, "correlation_id",
                              "identifier exceeds 128 bytes");
        for (i = 0; i < n; ++i)
            if ((unsigned char)r->impl->correlation_storage[i] < 32 ||
                (unsigned char)r->impl->correlation_storage[i] == 127)
                return knurl_fail(error, KNURL_INVALID, "correlation_id",
                                  "identifier contains a control byte");
        r->impl->correlation_storage[n] = 0;
        r->impl->correlation_id.data = r->impl->correlation_storage;
        r->impl->correlation_id.length = n;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_request_autoblock_context(knurl_request *request,
                                     knurl_autoblock_event *event)
{
    knurl_bytes header;
    char selected[46], swap[46];
    size_t i, j;
    event->address_source =
        (knurl_client_address_source)request->impl->address_source;
    event->peer_port = request->impl->peer.port;
    event->local_port = request->impl->local.port;
    event->listener_index = request->impl->listener;
    event->tls = request->impl->tls;
    event->process = request->impl->server->impl->process;
    if (request->impl->peer.address.data != NULL)
        strcpy(event->peer_address, request->impl->peer.address.data);
    if (request->impl->local.address.data != NULL)
        strcpy(event->local_address, request->impl->local.address.data);
    if (event->address_source == KNURL_CLIENT_ADDRESS_FORWARDED_FOR ||
        (event->address_source != KNURL_CLIENT_ADDRESS_PEER &&
         strcmp(event->peer_address, event->address) != 0))
        via_add(event, event->peer_address);
    if (event->address_source == KNURL_CLIENT_ADDRESS_FORWARDED_FOR &&
        knurl_request_header_first(request, "x-forwarded-for", &header, NULL) ==
            KNURL_OK)
        (void)forwarded(&request->impl->server->impl->config.client_ip, header,
                        selected, event);
    for (i = 0; i < event->via_count / 2; ++i) {
        j = event->via_count - 1 - i;
        strcpy(swap, event->via[i]);
        strcpy(event->via[i], event->via[j]);
        strcpy(event->via[j], swap);
    }
}
