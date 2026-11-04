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
static int trusted(const rotmos_client_ip_config *config, const char *value)
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
static int forwarded(const rotmos_client_ip_config *config, rotmos_bytes header,
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
rotmos_status rotmos_request_identity(rotmos_request *r, rotmos_error *error)
{
    const rotmos_server_config *config = &r->server->config;
    rotmos_bytes header;
    rotmos_status status;
    size_t n = 0, i;
    unsigned char bytes[16];
    char resolved[46];
    int found = 0;
    r->effective_address = r->peer.address;
    if (r->peer.address.data != NULL &&
        trusted(&config->client_ip, r->peer.address.data)) {
        if (rotmos_request_header_first(r, "x-forwarded-for", &header, NULL) ==
            ROTMOS_OK)
            found = forwarded(&config->client_ip, header, r->effective_storage);
        if (!found && rotmos_request_header_first(r, "x-real-ip", &header,
                                                  NULL) == ROTMOS_OK)
            found =
                token(header.data, (const char *)header.data + header.length,
                      r->effective_storage);
        if (found)
            r->effective_address = rotmos_view(r->effective_storage);
    }
    if (config->client_ip.resolve != NULL) {
        status = config->client_ip.resolve(r, resolved, sizeof(resolved), &n,
                                           config->client_ip.userdata, error);
        if (status != ROTMOS_OK)
            return status;
        if (n == 0 || n >= sizeof(r->effective_storage) ||
            memchr(resolved, 0, n) != NULL)
            return rotmos_fail(error, ROTMOS_INVALID, "client_address",
                               "resolver returned an invalid address length");
        resolved[n] = 0;
        if (address(resolved, bytes) == 0)
            return rotmos_fail(
                error, ROTMOS_INVALID, "client_address",
                "resolver must return a numeric IPv4 or IPv6 address");
        memcpy(r->effective_storage, resolved, n + 1);
        r->effective_address = rotmos_view(r->effective_storage);
    }
    if (config->correlation_id.resolve != NULL) {
        n = 0;
        status = config->correlation_id.resolve(
            r, r->correlation_storage, sizeof(r->correlation_storage), &n,
            config->correlation_id.userdata, error);
        if (status != ROTMOS_OK)
            return status;
        if (n > ROTMOS_CORRELATION_ID_MAX)
            return rotmos_fail(error, ROTMOS_LIMIT, "correlation_id",
                               "identifier exceeds 128 bytes");
        for (i = 0; i < n; ++i)
            if ((unsigned char)r->correlation_storage[i] < 32 ||
                (unsigned char)r->correlation_storage[i] == 127)
                return rotmos_fail(error, ROTMOS_INVALID, "correlation_id",
                                   "identifier contains a control byte");
        r->correlation_storage[n] = 0;
        r->correlation_id.data = r->correlation_storage;
        r->correlation_id.length = n;
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
