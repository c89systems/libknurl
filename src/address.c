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
static int forwarded(const knurl_client_ip_config *config, knurl_bytes header,
                     char out[46])
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
    if (r->impl->peer.address.data != NULL &&
        trusted(&config->client_ip, r->impl->peer.address.data)) {
        if (knurl_request_header_first(r, "x-forwarded-for", &header, NULL) ==
            KNURL_OK)
            found = forwarded(&config->client_ip, header,
                              r->impl->effective_storage);
        if (!found && knurl_request_header_first(r, "x-real-ip", &header,
                                                 NULL) == KNURL_OK)
            found =
                token(header.data, (const char *)header.data + header.length,
                      r->impl->effective_storage);
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
