#include "forwarded.h"
#include <string.h>
#include <strings.h>
static int token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
static void whitespace(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t'))
        ++*p;
}
static int numeric(char *value, knurl_mmdb_address *out, int forwarded)
{
    knurl_mmdb_address raw;
    char *end, *port = NULL, *address = value;
    unsigned long number = 0;
    if (forwarded) {
        if (*value == '[') {
            end = strchr(value, ']');
            if (end == NULL || (end[1] != 0 && end[1] != ':'))
                return 0;
            if (end[1] == ':')
                port = end + 2;
            *end = 0;
            address = value + 1;
            if (strchr(address, ':') == NULL)
                return 0;
        } else {
            end = strchr(value, ':');
            if (end != NULL) {
                *end = 0;
                port = end + 1;
            }
        }
        if (port != NULL) {
            if (*port == 0)
                return 0;
            while (*port) {
                if (*port < '0' || *port > '9' || number > 65535UL)
                    return 0;
                number = number * 10 + (unsigned long)(*port++ - '0');
            }
            if (number > 65535UL)
                return 0;
        }
    }
    return knurl_mmdb_address_parse(address, &raw, NULL) == KNURL_OK &&
           knurl_ip_normalize(&raw, out);
}
static int comma_chain(const char *p, const char *end, size_t limit,
                       struct knurl_forward_chain *out)
{
    const char *start, *stop;
    char value[46];
    size_t length;
    while (p < end) {
        whitespace(&p, end);
        start = p;
        while (p < end && *p != ',')
            ++p;
        stop = p;
        while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'))
            --stop;
        length = (size_t)(stop - start);
        if (length == 0 || length >= sizeof(value) || out->count == limit)
            return 0;
        memcpy(value, start, length);
        value[length] = 0;
        if (!numeric(value, &out->hops[out->count++], 0))
            return 0;
        if (p < end && ++p == end)
            return 0;
    }
    return out->count != 0;
}
static int rfc_chain(const char *p, const char *end, size_t limit,
                     struct knurl_forward_chain *out)
{
    const char *names[32], *name;
    size_t lengths[32], length, count, i, used;
    char value[256];
    int found, quoted;
    while (p < end) {
        found = 0;
        count = 0;
        if (out->count == limit)
            return 0;
        for (;;) {
            whitespace(&p, end);
            name = p;
            while (p < end && token_char((unsigned char)*p))
                ++p;
            length = (size_t)(p - name);
            if (!length || count == 32 || p == end || *p++ != '=')
                return 0;
            for (i = 0; i < count; ++i)
                if (lengths[i] == length &&
                    strncasecmp(names[i], name, length) == 0)
                    return 0;
            names[count] = name;
            lengths[count++] = length;
            used = 0;
            quoted = p < end && *p == '"';
            if (quoted) {
                ++p;
                while (p < end && *p != '"') {
                    if (*p == '\\' && ++p == end)
                        return 0;
                    if ((unsigned char)*p < 32 || (unsigned char)*p == 127 ||
                        used + 1 >= sizeof(value))
                        return 0;
                    value[used++] = *p++;
                }
                if (p == end)
                    return 0;
                ++p;
            } else {
                while (p < end && token_char((unsigned char)*p)) {
                    if (used + 1 >= sizeof(value))
                        return 0;
                    value[used++] = *p++;
                }
                if (!used)
                    return 0;
            }
            value[used] = 0;
            if (length == 3 && strncasecmp(name, "for", 3) == 0) {
                if (!numeric(value, &out->hops[out->count], 1))
                    return 0;
                found = 1;
            }
            whitespace(&p, end);
            if (p == end || *p == ',')
                break;
            if (*p++ != ';')
                return 0;
        }
        if (!found)
            return 0;
        ++out->count;
        if (p < end && ++p == end)
            return 0;
    }
    return out->count != 0;
}
int knurl_forwarded_parse(const knurl_client_ip_config *config,
                          knurl_bytes input, struct knurl_forward_chain *out)
{
    const char *p = input.data;
    size_t limit = config->max_hops ? config->max_hops : 16;
    out->count = 0;
    if (p == NULL || input.length == 0 || limit > 64 ||
        memchr(p, 0, input.length))
        return 0;
    return config->forwarded_format == KNURL_FORWARDED_RFC7239
               ? rfc_chain(p, p + input.length, limit, out)
               : comma_chain(p, p + input.length, limit, out);
}
