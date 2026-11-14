/* Trusted-chain resolution lifted from Vectis's Kore bridge. */
#define _POSIX_C_SOURCE 200809L
#include "policy_internal.h"
#include "server_internal.h"
#include "forwarded.h"
#include <arpa/inet.h>
#include <string.h>
#include <strings.h>
static int address(const char *value, unsigned char bytes[16])
{
    if (inet_pton(AF_INET, value, bytes) == 1)
        return 4;
    if (inet_pton(AF_INET6, value, bytes) == 1)
        return 16;
    return 0;
}
static int trusted(const knurl_client_ip_config *config,
                   const knurl_mmdb_address *value, int immediate)
{
    struct knurl_ip_network network;
    size_t i;
    for (i = 0; i < config->trusted_proxy_count; ++i) {
        if (strcmp(config->trusted_proxies[i], "*") == 0) {
            if (immediate)
                return 1;
        } else if (knurl_ip_network_parse(config->trusted_proxies[i],
                                          &network) &&
                   knurl_ip_contains(&network, value))
            return 1;
    }
    return 0;
}
static int trusted_peer(const knurl_client_ip_config *config, const char *value)
{
    knurl_mmdb_address raw, normalized;
    return value != NULL &&
           knurl_mmdb_address_parse(value, &raw, NULL) == KNURL_OK &&
           knurl_ip_normalize(&raw, &normalized) &&
           trusted(config, &normalized, 1);
}
/* 0 absent, 1 unique, -1 duplicate. Never combine untrusted identity fields. */
static int unique_header(knurl_request *r, const char *name, knurl_bytes *out)
{
    knurl_header field;
    size_t i, length;
    int found = 0;
    if (name == NULL || name[0] == 0)
        return 0;
    length = strlen(name);
    for (i = 0; i < knurl_request_header_count(r); ++i) {
        (void)knurl_request_header_at(r, i, &field, NULL);
        if (field.name.length == length &&
            strncasecmp(field.name.data, name, length) == 0) {
            if (found)
                return -1;
            *out = field.value;
            found = 1;
        }
    }
    return found;
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
    struct knurl_forward_chain chain;
    size_t i;
    if (!knurl_forwarded_parse(config, header, &chain))
        return 0;
    for (i = chain.count; i != 0; --i) {
        if (inet_ntop(chain.hops[i - 1].size == 4 ? AF_INET : AF_INET6,
                      chain.hops[i - 1].bytes, out, 46) == NULL)
            return 0;
        if (!trusted(config, &chain.hops[i - 1], 0) || i == 1)
            return 1;
        if (event != NULL)
            via_add(event, out);
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
    int found = 0, field;
    r->impl->effective_address = r->impl->peer.address;
    r->impl->address_source = KNURL_CLIENT_ADDRESS_PEER;
    r->impl->forwarding_invalid = 0;
    if (r->impl->peer.address.data != NULL &&
        trusted_peer(&config->client_ip, r->impl->peer.address.data)) {
        field = unique_header(r, config->client_ip.forwarded_header, &header);
        if (field == 1)
            found = forwarded(&config->client_ip, header,
                              r->impl->effective_storage, NULL);
        if (found)
            r->impl->address_source = KNURL_CLIENT_ADDRESS_FORWARDED_FOR;
        if (field == 0) {
            field = unique_header(r, config->client_ip.real_ip_header, &header);
            if (field == 1) {
                found = token(header.data,
                              (const char *)header.data + header.length,
                              r->impl->effective_storage);
                if (found)
                    r->impl->address_source = KNURL_CLIENT_ADDRESS_REAL_IP;
            }
        }
        r->impl->forwarding_invalid = field != 0 && !found;
        if (found)
            r->impl->effective_address = knurl_view(r->impl->effective_storage);
    }
    if (config->client_ip.resolve != NULL) {
        status = config->client_ip.resolve(r, resolved, sizeof(resolved), &n,
                                           config->client_ip.userdata, error);
        if (status != KNURL_OK)
            return status;
        if (n != 0) {
            if (n >= sizeof(r->impl->effective_storage) ||
                memchr(resolved, 0, n) != NULL)
                return knurl_fail(
                    error, KNURL_INVALID, "client_address",
                    "resolver returned an invalid address length");
            resolved[n] = 0;
            if (address(resolved, bytes) == 0)
                return knurl_fail(
                    error, KNURL_INVALID, "client_address",
                    "resolver must return a numeric IPv4 or IPv6 address");
            r->impl->address_source = KNURL_CLIENT_ADDRESS_RESOLVER;
            r->impl->forwarding_invalid = 0;
            memcpy(r->impl->effective_storage, resolved, n + 1);
            r->impl->effective_address = knurl_view(r->impl->effective_storage);
        }
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
        unique_header(
            request,
            request->impl->server->impl->config.client_ip.forwarded_header,
            &header) == 1)
        (void)forwarded(&request->impl->server->impl->config.client_ip, header,
                        selected, event);
    for (i = 0; i < event->via_count / 2; ++i) {
        j = event->via_count - 1 - i;
        strcpy(swap, event->via[i]);
        strcpy(event->via[i], event->via[j]);
        strcpy(event->via[j], swap);
    }
}
