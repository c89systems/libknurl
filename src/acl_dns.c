#define _POSIX_C_SOURCE 200809L
#include "acl_internal.h"
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
static int compare(const void *left, const void *right)
{
    const struct knurl_acl_resolved *a = left, *b = right;
    int order = knurl_ip_compare(&a->address, &b->address);
    return order ? order : a->deny - b->deny;
}
knurl_status knurl_acl_resolve_system(const char *hostname,
                                      knurl_mmdb_address *out, size_t capacity,
                                      size_t *count, void *userdata,
                                      knurl_error *error)
{
    struct addrinfo hints, *result, *at;
    knurl_mmdb_address raw, address;
    size_t i;
    int code;
    knurl_status status = KNURL_OK;
    (void)userdata;
    *count = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    code = getaddrinfo(hostname, NULL, &hints, &result);
    if (code != 0) {
        status =
            knurl_fail(error, KNURL_BACKEND, "acl.resolve", gai_strerror(code));
        if (error != NULL) {
            error->source = KNURL_ERROR_SYSTEM;
            error->native_code = code;
        }
        return status;
    }
    for (at = result; at != NULL; at = at->ai_next) {
        memset(&raw, 0, sizeof(raw));
        if (at->ai_family == AF_INET &&
            at->ai_addrlen >= sizeof(struct sockaddr_in)) {
            raw.size = 4;
            memcpy(raw.bytes, &((struct sockaddr_in *)at->ai_addr)->sin_addr,
                   4);
        } else if (at->ai_family == AF_INET6 &&
                   at->ai_addrlen >= sizeof(struct sockaddr_in6)) {
            raw.size = 16;
            memcpy(raw.bytes, &((struct sockaddr_in6 *)at->ai_addr)->sin6_addr,
                   16);
        } else
            continue;
        (void)knurl_ip_normalize(&raw, &address);
        for (i = 0; i < *count; ++i)
            if (out[i].size == address.size &&
                memcmp(out[i].bytes, address.bytes, address.size) == 0)
                break;
        if (i < *count)
            continue;
        if (*count == capacity) {
            status = knurl_fail(error, KNURL_LIMIT, "acl.resolve",
                                "hostname exceeds configured address capacity");
            break;
        }
        out[(*count)++] = address;
    }
    freeaddrinfo(result);
    if (status == KNURL_OK && *count == 0)
        status = knurl_fail(error, KNURL_BACKEND, "acl.resolve",
                            "hostname has no IPv4/IPv6 addresses");
    return status;
}
knurl_status knurl_acl_resolve_all(knurl_acl *acl,
                                   struct knurl_acl_resolved **out,
                                   size_t *out_count, int (*cancelled)(void *),
                                   void *userdata, knurl_error *error)
{
    struct knurl_acl_impl *impl = acl->impl;
    struct slice {
        size_t start, count;
    } *slices = NULL;
    struct knurl_acl_resolved *result = NULL, *grown;
    knurl_mmdb_address *addresses = NULL, normalized;
    size_t i, j, k, count, used = 0, capacity = impl->config.dns_max_addresses;
    knurl_status status = KNURL_OK;
    *out = NULL;
    *out_count = 0;
    for (i = 0; i < impl->entry_count; ++i) {
        if (impl->entries[i].hostname == NULL)
            continue;
        if (cancelled != NULL && cancelled(userdata)) {
            status = knurl_fail(error, KNURL_TIMEOUT, "acl.resolve",
                                "DNS refresh cancelled before next hostname");
            break;
        }
        if (addresses == NULL &&
            (addresses = malloc(capacity * sizeof(*addresses))) == NULL) {
            status = knurl_fail(error, KNURL_NOMEM, "acl.resolve",
                                "cannot allocate address scratch buffer");
            break;
        }
        if (slices == NULL &&
            (slices = calloc(impl->entry_count, sizeof(*slices))) == NULL) {
            status = knurl_fail(error, KNURL_NOMEM, "acl.resolve",
                                "cannot allocate hostname snapshot index");
            break;
        }
        for (k = 0; k < i; ++k)
            if (impl->entries[k].hostname != NULL &&
                strcasecmp(impl->entries[k].hostname,
                           impl->entries[i].hostname) == 0)
                break;
        count = 0;
        if (k == i)
            status = impl->config.resolve(impl->entries[i].hostname, addresses,
                                          capacity, &count,
                                          impl->config.resolve_userdata, error);
        else
            count = slices[k].count;
        if (status != KNURL_OK) {
            if (status == KNURL_AGAIN || status == KNURL_EOF)
                status = KNURL_BACKEND;
            break;
        }
        if (count == 0 || count > capacity) {
            status = knurl_fail(
                error, count ? KNURL_LIMIT : KNURL_BACKEND, "acl.resolve",
                "resolver returned an empty or oversized address set");
            break;
        }
        grown = realloc(result, (used + count) * sizeof(*result));
        if (grown == NULL) {
            status = knurl_fail(error, KNURL_NOMEM, "acl.resolve",
                                "cannot allocate resolved address snapshot");
            break;
        }
        result = grown;
        slices[i].start = used;
        slices[i].count = count;
        for (j = 0; j < count; ++j) {
            if (k < i)
                normalized = result[slices[k].start + j].address;
            else if (!knurl_ip_normalize(&addresses[j], &normalized)) {
                status = knurl_fail(error, KNURL_BACKEND, "acl.resolve",
                                    "resolver returned an invalid address");
                break;
            }
            result[used].address = normalized;
            result[used++].deny = impl->entries[i].deny;
        }
        if (status != KNURL_OK)
            break;
    }
    free(addresses);
    free(slices);
    if (status == KNURL_OK) {
        if (used > 1) {
            qsort(result, used, sizeof(*result), compare);
            count = 1;
            for (i = 1; i < used; ++i)
                if (compare(&result[i], &result[count - 1]) != 0)
                    result[count++] = result[i];
            used = count;
        }
        *out = result;
        *out_count = used;
    } else {
        free(result);
        if (error == NULL || error->status != status)
            (void)knurl_fail(error, status, "acl.resolve",
                             "hostname resolution failed");
        if (error != NULL && error->detail[0] == 0 && i < impl->entry_count)
            (void)snprintf(error->detail, sizeof(error->detail), "hostname=%s",
                           impl->entries[i].hostname);
    }
    return status;
}
